// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of k27 0061/0062's piano_pm_policy.h (the decisions piano_pm.c takes without hardware): the idle state choice with
// its domains and latency holds, the PSCI and LLCC verdicts, the wake sources, the Type-C address and the wake calendar.
// tooling/k27/test-piano-pm.sh builds and runs it and checks that the patch carries this header byte for byte.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "piano_pm_policy.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

/* Times in microseconds here: residency[l] = the table's min-residency. */
static uint64_t res_e[PIANO_PM_LEVELS], res_p[PIANO_PM_LEVELS];
static uint8_t none_disabled[PIANO_PM_LEVELS];

static struct piano_pm_pick
pick(const struct piano_pm_state *table, const uint64_t *res, int max, uint64_t idle_us)
{
	struct piano_pm_pick p;
	memset(&p, 0, sizeof(p));
	p.table = table;
	p.residency = res;
	p.disabled = none_disabled;
	p.max = max;
	p.now = 1000000;
	p.until = p.now + idle_us;
	p.latency_us = UINT32_MAX;
	p.cluster_mask = 0x3f;          /* cores 0-5 */
	p.all_mask = 0xff;
	p.idle = 0x01;                  /* only this CPU (cpu0) */
	p.cluster_until = p.all_until = p.until;
	p.ss3_ok = true;
	return p;
}

static void
test_cpu_state(void)
{
	struct piano_pm_pick p = pick(piano_pm_states_e, res_e, 1, 559);
	CHECK(piano_pm_pick_cpu(&p) == 0, "559 us < ret's 560 us min-residency: WFI");
	p = pick(piano_pm_states_e, res_e, 1, 560);
	CHECK(piano_pm_pick_cpu(&p) == 1, "560 us: ret");
	p = pick(piano_pm_states_p, res_p, 1, 600);
	CHECK(piano_pm_pick_cpu(&p) == 0, "a P core needs 686 us");
	p = pick(piano_pm_states_e, res_e, 0, 100000);
	CHECK(piano_pm_pick_cpu(&p) == 0, "level 0: always WFI");
	p = pick(piano_pm_states_e, res_e, 3, 100000);
	p.latency_us = 100;
	CHECK(piano_pm_pick_cpu(&p) == 0, "a 100 us hold excludes ret (129 us exit)");
	p.latency_us = 129;
	CHECK(piano_pm_pick_cpu(&p) == 1, "a 129 us hold allows ret");
	uint8_t disabled[PIANO_PM_LEVELS] = { 0, 1, 0, 0 };
	p = pick(piano_pm_states_e, res_e, 3, 100000);
	p.disabled = disabled;
	CHECK(piano_pm_pick_cpu(&p) == 0, "ret disabled: WFI");
	p = pick(piano_pm_states_e, res_e, 1, 100000);
	p.until = p.now - 1;
	CHECK(piano_pm_pick_cpu(&p) == 0, "a deadline already past: WFI");
	p.until = UINT64_MAX;
	CHECK(piano_pm_pick_cpu(&p) == 1, "no deadline at all: ret");
}

static void
test_domains(void)
{
	uint32_t param = 0;
	struct piano_pm_pick p = pick(piano_pm_states_e, res_e, 3, 100000);
	CHECK(piano_pm_pick_domain(&p, &param) == 1 && param == 0x4, "cluster not complete: ret (0x4), got param %#x", param);
	p.idle = 0x3f;
	CHECK(piano_pm_pick_domain(&p, &param) == 2 && param == 0x01000054, "cluster 0 complete: cl5, got %#x", param);
	p.idle = 0xff;
	CHECK(piano_pm_pick_domain(&p, &param) == 3 && param == 0x0200c354, "all CPUs idle: ss3, got %#x", param);
	p.ss3_ok = false;
	CHECK(piano_pm_pick_domain(&p, &param) == 2 && param == 0x01000054, "LLCC island not reserved: cl5 at most");
	p.ss3_ok = true;
	p.max = 2;
	CHECK(piano_pm_pick_domain(&p, &param) == 2, "level 2: cl5 at most");
	p.max = 3;
	p.latency_us = 2000;
	CHECK(piano_pm_pick_domain(&p, &param) == 2, "PianoTouch's 2000 us hold: cl5 (1983 us exit) but not ss3 (4400 us)");
	p.latency_us = 1500;
	CHECK(piano_pm_pick_domain(&p, &param) == 1 && param == 0x4, "a 1500 us hold: ret only");
	p.latency_us = UINT32_MAX;
	p.cluster_until = p.now + 9000;
	CHECK(piano_pm_pick_domain(&p, &param) == 1, "a cluster CPU due in 9 ms < cl5's 9144 us: ret");
	p.cluster_until = p.now + 9144;
	p.all_until = p.now + 10000;
	CHECK(piano_pm_pick_domain(&p, &param) == 2, "the system due in 10 ms < ss3's 10150 us: cl5");
	/* cluster 1 (the P cores 6-7) completing while cluster 0 is busy */
	p = pick(piano_pm_states_p, res_p, 3, 100000);
	p.cluster_mask = 0xc0;
	p.idle = 0xc0;
	CHECK(piano_pm_pick_domain(&p, &param) == 2 && param == 0x01000054, "cluster 1 complete, cluster 0 busy: cl5");
	uint8_t disabled[PIANO_PM_LEVELS] = { 0, 0, 1, 0 };
	p.disabled = disabled;
	p.idle = 0xff;
	CHECK(piano_pm_pick_domain(&p, &param) == 1, "cl5 disabled: no domain state at all");
}

static void
test_verdicts(void)
{
	CHECK(piano_pm_osi_supported(3), "FEATURES 3: OSI");
	CHECK(!piano_pm_osi_supported(2), "FEATURES 2: extended StateID only");
	CHECK(!piano_pm_osi_supported(-1), "NOT_SUPPORTED");
	CHECK(piano_pm_llcc_island_ok(0x7fffffff, 0), "the downstream table");
	CHECK(!piano_pm_llcc_island_ok(0, 0x7fffffff), "the upstream table: bonus ways only");
	CHECK(!piano_pm_llcc_island_ok(0x7fffffff, 0x1), "reserved and bonus");
	for (int l = 1; l < PIANO_PM_LEVELS; l++) {
		CHECK((piano_pm_states_e[l].param & PIANO_PM_POWER_DOWN) == 0 && (piano_pm_states_p[l].param & PIANO_PM_POWER_DOWN) == 0,
		    "level %d is a retention state", l);
		CHECK(piano_pm_states_e[l].exit_us + piano_pm_states_e[l].entry_us <= piano_pm_states_e[l].residency_us,
		    "level %d: entry + exit within its min-residency", l);
	}
}

static void
test_wake(void)
{
	struct piano_pm_wake b;
	/* power key up at entry: a press wakes */
	piano_pm_wake_arm(&b, PIANO_SRC_POWER, true, 0x00, true, 1, true, 0, 0, false, 0);
	CHECK(b.src == PIANO_SRC_POWER, "power key armed, got %#x", b.src);
	CHECK(piano_pm_wake_check(&b, true, 0x00, 0, 0, false, 0) == 0, "nothing pressed");
	CHECK(piano_pm_wake_check(&b, false, 0x80, 0, 0, false, 0) == 0, "an unreadable sample is no press");
	CHECK(piano_pm_wake_check(&b, true, 0x80, 0, 0, false, 0) == PIANO_PM_WAKE_POWER_KEY, "press");
	/* held at entry (the press that asked for sleep): only a new press wakes */
	piano_pm_wake_arm(&b, PIANO_SRC_POWER, true, 0x80, true, 1, true, 0, 0, false, 0);
	CHECK(piano_pm_wake_check(&b, true, 0x80, 0, 0, false, 0) == 0, "still held");
	CHECK(piano_pm_wake_check(&b, true, 0x00, 0, 0, false, 0) == 0, "released");
	CHECK(piano_pm_wake_check(&b, true, 0x80, 0, 0, false, 0) == PIANO_PM_WAKE_POWER_KEY, "pressed again");
	/* no PON reading: power and volume-down disarmed */
	piano_pm_wake_arm(&b, PIANO_SRC_POWER | PIANO_SRC_VOLUME_DOWN, false, 0, true, 1, true, 0, 0, false, 0);
	CHECK(b.src == 0, "no PON: nothing armed, got %#x", b.src);
	/* volume-down only when asked for */
	piano_pm_wake_arm(&b, PIANO_SRC_POWER, true, 0, true, 1, true, 0, 0, false, 0);
	CHECK(piano_pm_wake_check(&b, true, 0x40, 0, 0, false, 0) == 0, "volume-down not armed");
	piano_pm_wake_arm(&b, PIANO_SRC_POWER | PIANO_SRC_VOLUME_DOWN, true, 0, true, 1, true, 0, 0, false, 0);
	CHECK(piano_pm_wake_check(&b, true, 0x40, 0, 0, false, 0) == PIANO_PM_WAKE_VOLUME_DOWN, "volume-down");
	/* the cover: armed only when closed at entry (hall low), wakes after two open readings */
	piano_pm_wake_arm(&b, PIANO_SRC_LID, true, 0, true, 1, true, 0, 0, false, 0);
	CHECK(!(b.src & PIANO_SRC_LID), "cover open at entry: no cover wake");
	piano_pm_wake_arm(&b, PIANO_SRC_LID, true, 0, true, 0, true, 0, 0, false, 0);
	CHECK(b.src & PIANO_SRC_LID, "cover closed at entry: armed");
	CHECK(piano_pm_wake_check(&b, true, 0, 1, 0, false, 0) == 0, "one open reading");
	CHECK(piano_pm_wake_check(&b, true, 0, 0, 0, false, 0) == 0, "closed again (a glitch)");
	CHECK(piano_pm_wake_check(&b, true, 0, 1, 0, false, 0) == 0, "open once more");
	CHECK(piano_pm_wake_check(&b, true, 0, 1, 0, false, 0) == PIANO_PM_WAKE_LID, "open twice in a row: wake");
	piano_pm_wake_arm(&b, PIANO_SRC_LID, true, 0, false, 0, true, 0, 0, false, 0);
	CHECK(!(b.src & PIANO_SRC_LID), "no hall reading: no cover wake");
	/* the keyboard's status line: cover open, raw status latched by its owner, nothing latched yet */
	piano_pm_wake_arm(&b, PIANO_SRC_KEYBOARD, true, 0, true, 1, true, PIANO_TLMM_RAW_STATUS | 0x8, 0, false, 0);
	CHECK(b.src & PIANO_SRC_KEYBOARD, "keyboard armed");
	CHECK(piano_pm_wake_check(&b, true, 0, 0, 0, false, 0) == 0, "no edge");
	CHECK(piano_pm_wake_check(&b, true, 0, 0, 1, false, 0) == PIANO_PM_WAKE_KEYBOARD, "a latched rising edge");
	piano_pm_wake_arm(&b, PIANO_SRC_KEYBOARD, true, 0, true, 0, true, PIANO_TLMM_RAW_STATUS, 0, false, 0);
	CHECK(!(b.src & PIANO_SRC_KEYBOARD), "cover closed: no keyboard wake");
	piano_pm_wake_arm(&b, PIANO_SRC_KEYBOARD, true, 0, true, 1, true, 0, 0, false, 0);
	CHECK(!(b.src & PIANO_SRC_KEYBOARD), "no raw status configured: no keyboard wake");
	piano_pm_wake_arm(&b, PIANO_SRC_KEYBOARD, true, 0, true, 1, true, PIANO_TLMM_RAW_STATUS, 1, false, 0);
	CHECK(!(b.src & PIANO_SRC_KEYBOARD), "an edge already latched: not armed");
	/* USB-C: any change of the attach bit */
	piano_pm_wake_arm(&b, PIANO_SRC_USB, true, 0, true, 1, true, 0, 0, true, 0x01);
	CHECK(b.src & PIANO_SRC_USB, "usb armed");
	CHECK(piano_pm_wake_check(&b, true, 0, 0, 0, true, 0x03) == 0, "still attached (CC bit changes nothing)");
	CHECK(piano_pm_wake_check(&b, true, 0, 0, 0, false, 0x00) == 0, "an unreadable sample is no change");
	CHECK(piano_pm_wake_check(&b, true, 0, 0, 0, true, 0x00) == PIANO_PM_WAKE_USB, "detached");
	piano_pm_wake_arm(&b, PIANO_SRC_USB, true, 0, true, 1, true, 0, 0, false, 0);
	CHECK(!(b.src & PIANO_SRC_USB), "no Type-C reading: not armed");
}

static void
test_typec_and_calendar(void)
{
	CHECK(piano_pm_charger_kind(0x02, 0x7f) && piano_pm_charger_kind(0x02, 0xa8) && piano_pm_charger_kind(0x02, 0xba), "kinds");
	CHECK(!piano_pm_charger_kind(0x02, 0x10) && !piano_pm_charger_kind(0x51, 0x7f), "not chargers");
	CHECK(piano_pm_typec_address(0x2600, 0x7f) == 0x2c00, "Type-C 0x600 above, got %#x", piano_pm_typec_address(0x2600, 0x7f));
	CHECK(piano_pm_typec_address(0x2600, 0xba) == 0x2f00, "SUBTYPE 0xba: 0x900 above, got %#x",
	    piano_pm_typec_address(0x2600, 0xba));
	unsigned long s;
	unsigned int us;
	piano_pm_add_slept(1000, 999999, 1000, &s, &us);
	CHECK(s == 1001 && us == 0, "a microsecond carries into the seconds: %lu.%06u", s, us);
	piano_pm_add_slept(1800000000UL, 250000, 3723500000000ULL, &s, &us);
	CHECK(s == 1800003723UL && us == 750000, "an hour, two minutes, 3.5 s: %lu.%06u", s, us);
	piano_pm_add_slept(5, 600000, 1600000000ULL, &s, &us);
	CHECK(s == 7 && us == 200000, "1.6 s onto .6: %lu.%06u", s, us);
}

int
main(void)
{
	for (int l = 0; l < PIANO_PM_LEVELS; l++) {
		res_e[l] = piano_pm_states_e[l].residency_us;
		res_p[l] = piano_pm_states_p[l].residency_us;
	}
	test_cpu_state();
	test_domains();
	test_verdicts();
	test_wake();
	test_typec_and_calendar();
	if (failures) {
		printf("test_piano_pm_policy: %d failure(s)\n", failures);
		return 1;
	}
	printf("test_piano_pm_policy: all checks passed\n");
	return 0;
}
