// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery for macOS (piano-power-ps=1): an IOPMPowerSource whose values come from the two fuel gauges and the Type-C state that
// PianoPower reads at level 1 (BatteryMap.h). No DSP is needed and nothing is written to any hardware here.
//
// How the system finds it (xnu source, iokit/Kernel): IOPMrootDomain looks up services matching "IOPMPowerSource", and powerd
// (which feeds the menu bar's battery item) watches for that class too; updateStatus() copies the values into the registry and sends
// kIOPMMessageBatteryStatusHasChanged to its clients. The class is named AppleSmartBattery, the name a Mac's battery service has, so
// anything that looks the battery up by that class name or registry name finds this one; the vma2 kernel collection has no class of
// that name (it boots virtual machines, which have no battery), so there is no clash. Only setters of the public IOPMPowerSource
// interface and setPSProperty are used.
//
// Low charge: AtWarnLevel and AtCriticalLevel are never set here. The kernel's own low-battery sleep is started by
// kIOPMPowerEmergency from Apple's battery manager, which this is not; and every system sleep request is refused while the ADT's
// /defaults sleep-disabled is 1 (IOPMrootDomain::checkSystemSleepAllowed; configs/k27-adt.toml sets it), so a low percentage gives
// macOS's warnings but no sleep attempt on this tablet. docs/drivers/charging.md §5d.
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include "PianoBattery.h"

using namespace pianopower;

class AppleSmartBattery : public IOPMPowerSource {
  OSDeclareDefaultStructors(AppleSmartBattery)

  void number(const char *key, int64_t value)
  {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
    if (symbol && n) setPSProperty(symbol, n);
    OSSafeReleaseNULL(n);
    OSSafeReleaseNULL(symbol);
  }
  void flag(const char *key, bool value)
  {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
    OSSafeReleaseNULL(symbol);
  }
  void text(const char *key, const char *value)
  {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    OSString *s = OSString::withCString(value);
    if (symbol && s) setPSProperty(symbol, s);
    OSSafeReleaseNULL(s);
    OSSafeReleaseNULL(symbol);
  }

 public:
  void apply(const BatteryValues &v)
  {
    setBatteryInstalled(v.installed);
    setExternalConnected(v.external);
    setExternalChargeCapable(v.chargeCapable);
    setIsCharging(v.charging);
    setAtWarnLevel(false);
    setAtCriticalLevel(false);
    setCurrentCapacity(v.percent);
    setMaxCapacity(v.maxCapacity);
    setVoltage(v.voltageMv);
    setAmperage(v.amperageMa);
    setCycleCount(v.cycles);
    setTimeRemaining(int(v.timeRemaining));
    flag("FullyCharged", v.full);
    number("InstantAmperage", v.instantMa);
    number("DesignCapacity", v.designMah);
    number("AppleRawCurrentCapacity", v.rawMah);
    number("AppleRawMaxCapacity", v.fullMah);
    number("NominalChargeCapacity", v.fullMah);
    number("AvgTimeToEmpty", v.timeToEmpty);
    number("AvgTimeToFull", v.timeToFull);
    if (v.temperatureValid) number("Temperature", v.temperatureCenti);
    text("DeviceName", "bq27z561 x2");
    text("Manufacturer", "Texas Instruments");
    OSDictionary *adapter = OSDictionary::withCapacity(4);
    if (adapter) {
      if (v.adapter) {
        OSNumber *family = OSNumber::withNumber(v.adapterFamily, 32), *w = OSNumber::withNumber(v.adapterWatts, 32),
                 *mv = OSNumber::withNumber(v.adapterMv, 32), *ma = OSNumber::withNumber(v.adapterMa, 32);
        if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
        if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
        if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
        if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
      }
      const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
      if (key) setPSProperty(key, adapter);
      OSSafeReleaseNULL(key);
      adapter->release();
    }
  }
};

OSDefineMetaClassAndStructors(AppleSmartBattery, IOPMPowerSource)

namespace {
AppleSmartBattery *gBattery;
}

bool pianoBatteryUpdate(IOService *owner, const BatteryValues &v)
{
  if (!v.known) return gBattery != nullptr;   // no reading this cycle: what macOS sees stays as it was
  if (!gBattery) {
    if (!owner) return false;
    AppleSmartBattery *b = OSTypeAlloc(AppleSmartBattery);
    if (!b || !b->init()) {
      IOLog("PianoPower: battery: FAILED to create the power source\n");
      OSSafeReleaseNULL(b);
      return false;
    }
    // The values are in the registry before anyone can find the service.
    b->apply(v);
    b->updateStatus();
    if (!b->attach(owner)) {
      IOLog("PianoPower: battery: FAILED to attach the power source\n");
      b->release();
      return false;
    }
    b->registerService();
    gBattery = b;   // the attachment keeps it; this reference is the driver's
    IOLog("PianoPower: battery: AppleSmartBattery (IOPMPowerSource) published: %u %%, %s%s\n", v.percent,
          v.external ? (v.charging ? "on power, charging" : v.full ? "on power, full" : "on power, not charging") : "on battery",
          v.clamped ? " (shown at the floor)" : "");
    return true;
  }
  gBattery->apply(v);
  gBattery->updateStatus();
  return true;
}
