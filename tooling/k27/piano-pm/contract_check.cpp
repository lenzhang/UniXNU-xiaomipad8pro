// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compile check of package/include/PianoPowerContract.h as a kext sees it (tooling/k27/test-piano-pm.sh --kext): a minimal
// driver that joins the power tree, takes a latency hold around a transfer, asks for the last wake's reason and, when its
// hardware would not go down, cancels the suspend (k27 0064, through IOPMrootDomain: no kernel symbol). Not linked into anything.
#include <PianoPowerContract.h>

class PianoPowerContractCheck : public IOService {
	OSDeclareDefaultStructors(PianoPowerContractCheck);

public:
	bool start(IOService *provider) override;
	IOReturn setPowerState(unsigned long state, IOService *device) override;
	void transfer();
};

OSDefineMetaClassAndStructors(PianoPowerContractCheck, IOService)

bool
PianoPowerContractCheck::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	return pianopower::join(this) == kIOReturnSuccess;
}

IOReturn
PianoPowerContractCheck::setPowerState(unsigned long state, IOService *)
{
	if (state == pianopower::kOff && piano_sleep_enabled()) {
		IOLog("contract check: off (last wake %s)\n", piano_s2idle_reason_name(piano_s2idle_last_reason()));
		if (pianopower::cancelSystemSleep("PianoPowerContractCheck", "a compile check") != kIOReturnSuccess) {
			IOLog("contract check: the sleep goes on\n");
		}
	}
	return pianopower::kAckNow;
}

void
PianoPowerContractCheck::transfer()
{
	int hold = piano_cpuidle_latency_hold(2000);
	IODelay(10);
	piano_cpuidle_latency_release(hold);
}
