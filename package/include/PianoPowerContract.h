// SPDX-License-Identifier: BSD-2-Clause-Patent
// The piano power contract: what a native piano driver does for system sleep and idle, and what the kernel offers it
// (k27 0061 piano-cpuidle, 0062 piano-sleep = suspend-to-idle). docs/power.md §4 is the reference, with the list of what each
// driver adds; this header is its code side. Include it as <PianoPowerContract.h> (tooling/k27/build-native-kext.py puts
// package/include on every kext's include path).
//
// The sequence a driver sees (docs/power.md §3):
//   1. IOPMrootDomain decides to sleep (idle, Apple menu, lid via PianoKeyboard, power key via PianoInput, piano-sleep-test).
//   2. kIOMessageSystemWillSleep to interested clients; then every driver in the power tree gets setPowerState(kOff) and
//      acknowledges (here: returns kIOPMAckImplied once done, synchronously, within 1 s).
//   3. The kernel freezes: every CPU parked, no thread runs, no polling thread wakes, nothing of the driver runs at all until
//      the wake. mach_absolute_time() does not advance across it (as on a Mac), so the driver's timeouts and deadlines are
//      not consumed by the time asleep; hardware that changed meanwhile (cover, USB-C, charger) must be read again on wake.
//   4. The kernel itself polls the wake sources (power key, cover opening, keyboard status line, USB-C, a timer): a driver
//      arms nothing, except PianoKeyboard's gpio95 latch (below). The power key press that woke the tablet is held by the
//      kernel until released, so PianoInput sees no press.
//   5. setPowerState(kOn) for every driver, then kIOMessageSystemHasPoweredOn; a full wake (display on).
//
// What kOff means (each driver, for what it owns; PianoA830: below, "Votes other blocks depend on"): no activity that costs power
// (panel and backlight off, touch and keyboard scanning stopped, GPU in slumber, links in their low-power state), and every RPMh
// vote, interconnect bandwidth vote, clock
// and GDSC the driver holds released or at its floor: the APPS RSC replays its sleep set only when the CPU subsystem goes
// down, so any active vote left in place keeps CX and DDR up for the whole sleep (Ubuntu's v0.8.0 list; docs/ubuntu-history/
// kernel.md §2.1). Never touch a register block whose clocks the driver just gated (DWC3's QSCRATCH with the glue clocks off
// resets the SoC). Interrupts a driver enabled at the GIC: disabled in kOff unless they are its wake line.
#ifndef PIANO_POWER_CONTRACT_H
#define PIANO_POWER_CONTRACT_H

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSBoolean.h>
#include <pexpert/pexpert.h>

extern "C" {
// k27 0061/0062 (osfmk/arm64/piano_pm.c). The KC builder resolves these by name in the kernel: a kext that calls them links
// only against a kernel with 0061 (the latency hold) or 0062 (the rest), which tooling/k27/build-native-image.sh pins.

// While held, no CPU idle state slower than MAX_EXIT_US to leave is entered. PianoTouch's DMA (GPI) SPI reads: hold 2000 for
// the transfer (Linux holds the same around each GPI message: the CPU subsystem state's 4400 us exit stalls the DMA and
// washes out a frame). Returns a handle, -1 when all 8 slots are taken (then go on without one). Any context.
int piano_cpuidle_latency_hold(uint32_t max_exit_us);
void piano_cpuidle_latency_release(int handle);

// piano-sleep=1: system sleep is suspend-to-idle (false: the ADT's sleep-disabled holds and macOS never sleeps).
bool piano_sleep_enabled(void);
// The last wake's source, as a name ("power-key", "lid", "usb", "volume-down", "timer", "irq", "keyboard", "aborted").
int piano_s2idle_last_reason(void);
const char *piano_s2idle_reason_name(int reason);
}

namespace pianopower {

enum : unsigned long { kOff = 0, kOn = 1, kStateCount = 2 };

// The two states every piano driver registers: kOff (nothing it owns powered or voted) and kOn (usable; needs the system on).
inline IOPMPowerState *
powerStates()
{
	static IOPMPowerState states[kStateCount] = {
		{ kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
		{ kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
	};
	return states;
}

// In start(), once the hardware is up: joins the power tree where the platform expert puts the provider (as NativeGopDisplay
// does) and asks for kOn, so that IOPMrootDomain takes the driver down before a sleep and up after it. PMstop() in stop().
inline IOReturn
join(IOService *driver)
{
	driver->PMinit();
	IOService::getPlatform()->PMRegisterDevice(driver->getProvider(), driver);
	IOReturn r = driver->registerPowerDriver(driver, powerStates(), kStateCount);
	if (r == kIOReturnSuccess) {
		driver->changePowerStateTo(kOn);
	}
	return r;
}

// A driver whose kOff/kOn takes longer than 1 s returns the worst case in microseconds from setPowerState instead of
// kIOPMAckImplied and calls acknowledgeSetPowerState() when done (at most 30 s, or IOPMrootDomain's watchdog fires).
constexpr IOReturn kAckNow = kIOPMAckImplied;

// Votes other blocks depend on (2026-11-11, PianoA830's piano-gpu-suspend=1): the GPU's host CX/MX/DDR votes on apps DRV-2 are the
// only HLOS votes for those resources (UFS, the display and the CPU have none of their own), and the power tree's drivers go down in
// no fixed order. So PianoA830 powers the GPU down in its kOff and lowers those votes only in an IOPlatformSleepAction (IOCPUSleepKernel's
// PreSleep, after every driver of the power tree acknowledged kOff), puts them back in its IOPlatformWakeAction (PostResume, before any
// driver's kOn), and keeps CX/MX at NOM_L1 unless told otherwise (piano-gpu-suspend-rails): a driver that needs CX while suspended is
// covered by that floor; at 0 the rails' votes go and only blocks already off in their kOff are safe.
// Refusing a suspend: k27 0062 has no cancel a driver can ask for (its aborts are its own: a CPU that does not park). A driver that
// cannot go down puts itself back up as it was (never half down: no gated clock with its users still running, no vote dropped that its
// hardware still needs) and acknowledges; the suspend goes on without its saving. PianoA830 does so and stops taking part for the boot
// after a failed entry.

// The display's scanout (who reads the framebuffer from DDR while suspended): stopped only when PianoDpu's panel slice put the panel to
// sleep with the timing engines stopped (its "piano-dpu-panel" property "asleep", and piano-dpu-panel-engines not 0); running when the
// slice is there and awake, or PianoDpu says so with a "piano-display-scanout" boolean (preferred when present: true while an engine
// fetches); unknown without PianoDpu's slice: the boot framebuffer then scans as ABL left it, so treat unknown as running.
enum class Scanout : uint8_t { unknown = 0, running = 1, stopped = 2 };

inline Scanout
displayScanout(IOService *dpu)
{
	if (dpu == nullptr) {
		return Scanout::unknown;
	}
	if (OSBoolean *b = OSDynamicCast(OSBoolean, dpu->getProperty("piano-display-scanout"))) {
		return b->isTrue() ? Scanout::running : Scanout::stopped;
	}
	OSString *panel = OSDynamicCast(OSString, dpu->getProperty("piano-dpu-panel"));
	if (panel == nullptr) {
		return Scanout::unknown;
	}
	if (!panel->isEqualTo("asleep")) {
		return Scanout::running;
	}
	uint32_t engines = 1;
	if (!PE_parse_boot_argn("piano-dpu-panel-engines", &engines, sizeof(engines))) {
		engines = 1;
	}
	return engines != 0 ? Scanout::stopped : Scanout::running;
}

// The LLCC ISLAND1 slice (SCID 12) as CX collapse needs it (reserved ways 0x7fffffff, no bonus ways; docs/power.md §3.6): PianoA830
// owns the LLCC. It reads the slice at every GPU boot and programs it with piano-llcc-island=1; this asks it, reading the two words
// again (lock-free, any thread). False without PianoA830 or before its first reading. PianoAudio's release of the LPASS votes waits
// for it; k27 0061/0062 read the slice themselves at boot (their own probe, before any kext) and do not see a later programming.
inline bool
llccIslandReady()
{
	IOService *a830 = IOService::copyMatchingService(IOService::serviceMatching("PianoA830"));
	if (a830 == nullptr) {
		return false;
	}
	bool ready = false;
	const IOReturn r = a830->callPlatformFunction("piano-llcc-island-ready", false, &ready, nullptr, nullptr, nullptr);
	a830->release();
	return r == kIOReturnSuccess && ready;
}

// The keyboard's wake line (PianoKeyboard): before acknowledging kOff with the cover open, set TLMM gpio95 INTR_CFG to latch
// rising edges (raw status, bit 4; detection edge, polarity high; enable bit 0 clear: nothing reaches the GIC) and clear its
// INTR_STATUS; the kernel then wakes on the latch (piano-sleep-wake bit 16). Clear INTR_STATUS again in kOn. Never gpio97.
constexpr uint32_t kKeyboardWakeGpio = 95;

} // namespace pianopower

#endif
