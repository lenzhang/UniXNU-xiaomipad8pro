// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as macOS reads it (piano-power-ps=1): an IOPMPowerSource published under PianoPower and refreshed on each report
// cycle (PianoBattery.cpp). The values come from BatteryMap.h.
#ifndef PIANO_POWER_BATTERY_H
#define PIANO_POWER_BATTERY_H
#include <IOKit/IOService.h>
#include "BatteryMap.h"

// Publishes the power source on the first call with a reading (nothing before: 0 % would read as an empty battery) and updates it
// afterwards; a call without a reading changes nothing. owner: the service it is attached under. Returns true once published.
bool pianoBatteryUpdate(IOService *owner, const pianopower::BatteryValues &v);

#endif
