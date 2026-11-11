// SPDX-License-Identifier: BSD-2-Clause-Patent
// Following the panel's display power without polling (docs/policy.md 5.6, docs/power.md "Idle wakeups by driver"):
// NativeGopDisplay publishes "piano-display-power" (boolean) on every power change call and messages
// kIOMessageServicePropertyChange (argument: the property's name) on each change; its "piano-display-power-messages" = true says so
// before it registers. A driver that wants the display's state keeps one pianodisplay::Follow, calls start() once (thread context,
// after its own setup), and reads state() whenever it needs it (lock-free, any context); an optional callback runs on each change.
//
// The callback runs in the sender's context: NativeGopDisplay's gated power change (its work loop). It may only record (atomics,
// thread_wakeup, IOLockWakeup with a lock the driver never holds while calling NativeGopDisplay): never call back into
// NativeGopDisplay, never block, never print at length.
//
// The panel's instance is preferred: one with piano-display-power-messages replaces one without (D6's second instance,
// piano-dp-fake, shares the class and publishes no display power). Without the property (a NativeGopDisplay from before 2026-11-12)
// the messages still arrive if it sends them (from 2026-11-11); state() is then what the property says, read at each message and at
// start. Use it from one translation unit of a kext (its functions are inline).
#ifndef PIANO_DISPLAY_FOLLOW_H
#define PIANO_DISPLAY_FOLLOW_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <string.h>

namespace pianodisplay {

enum : int { kUnknown = -1, kOff = 0, kOn = 1 };
constexpr const char *kPowerKey = "piano-display-power";
constexpr const char *kMessagesKey = "piano-display-power-messages";

// The property as a state: -1 absent (no NativeGopDisplay, or before WindowServer's first power call), 0 off, 1 on.
inline int
readState(IOService *display)
{
	if (display == nullptr) {
		return kUnknown;
	}
	OSObject *o = display->copyProperty(kPowerKey);
	int r = kUnknown;
	if (OSBoolean *b = OSDynamicCast(OSBoolean, o)) {
		r = b->isTrue() ? kOn : kOff;
	} else if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
		r = n->unsigned32BitValue() ? kOn : kOff;
	}
	OSSafeReleaseNULL(o);
	return r;
}

inline bool
marked(IOService *display)
{
	OSObject *o = display ? display->copyProperty(kMessagesKey) : nullptr;
	const bool m = OSDynamicCast(OSBoolean, o) && static_cast<OSBoolean *>(o)->isTrue();
	OSSafeReleaseNULL(o);
	return m;
}

class Follow {
public:
	using Callback = void (*)(void *context, int state);

	// Once, thread context. False when nothing could be set up (no lock, no notification): state() then stays unknown.
	bool
	start(Callback callback, void *context)
	{
		if (lock_ != nullptr) {
			return true;
		}
		callback_ = callback;
		context_ = context;
		lock_ = IOLockAlloc();
		if (lock_ == nullptr) {
			return false;
		}
		OSDictionary *match = IOService::serviceMatching("NativeGopDisplay");
		if (match == nullptr) {
			return false;
		}
		notifier_ = IOService::addMatchingNotification(gIOPublishNotification, match, &Follow::published, this, nullptr);
		match->release();
		return notifier_ != nullptr;
	}

	int state() const { return __atomic_load_n(&state_, __ATOMIC_ACQUIRE); }
	bool dark() const { return state() == kOff; }
	bool found() const { return __atomic_load_n(&found_, __ATOMIC_ACQUIRE) != 0; }
	bool evented() const { return __atomic_load_n(&marked_, __ATOMIC_ACQUIRE) != 0 || changes() != 0; }
	uint32_t changes() const { return __atomic_load_n(&messages_, __ATOMIC_ACQUIRE); }

private:
	static bool
	published(void *target, void *, IOService *service, IONotifier *)
	{
		static_cast<Follow *>(target)->adopt(service);
		return true;
	}

	static IOReturn
	interest(void *target, void *, UInt32 type, IOService *provider, void *arg, vm_size_t)
	{
		if (type != kIOMessageServicePropertyChange) {
			return kIOReturnUnsupported;
		}
		const char *name = static_cast<const char *>(arg);
		if (name == nullptr || strcmp(name, kPowerKey) != 0) {
			return kIOReturnSuccess;
		}
		Follow *self = static_cast<Follow *>(target);
		__atomic_add_fetch(&self->messages_, 1u, __ATOMIC_ACQ_REL);
		self->update(readState(provider));
		return kIOReturnSuccess;
	}

	void
	update(int s)
	{
		const int before = __atomic_exchange_n(&state_, s, __ATOMIC_ACQ_REL);
		if (before != s && callback_ != nullptr) {
			callback_(context_, s);
		}
	}

	// The first NativeGopDisplay, or the panel's (marked) in place of an unmarked one. Interest first, then the first read.
	void
	adopt(IOService *service)
	{
		const bool m = marked(service);
		IOLockLock(lock_);
		if (service == display_ || (display_ != nullptr && (marked_ || !m))) {
			IOLockUnlock(lock_);
			return;
		}
		IOService *old = display_;
		IONotifier *oldInterest = interest_;
		service->retain();
		display_ = service;
		__atomic_store_n(&marked_, m ? 1u : 0u, __ATOMIC_RELEASE);
		interest_ = service->registerInterest(gIOGeneralInterest, &Follow::interest, this, nullptr);
		IOLockUnlock(lock_);
		if (oldInterest != nullptr) {
			oldInterest->remove();
		}
		OSSafeReleaseNULL(old);
		__atomic_store_n(&found_, 1u, __ATOMIC_RELEASE);
		update(readState(service));
	}

	IOLock *lock_ = nullptr;
	IONotifier *notifier_ = nullptr, *interest_ = nullptr;
	IOService *display_ = nullptr;
	Callback callback_ = nullptr;
	void *context_ = nullptr;
	int state_ = kUnknown;
	uint32_t marked_ = 0, found_ = 0, messages_ = 0;
};

} // namespace pianodisplay

#endif
