// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbHost levels 3 and 4 (docs/drivers/usb-host.md H3/H4): what the port's witnesses say together, and the role switch between
// PianoDwc3's device mode and our host mode. Portable, no I/O (host tests in tests/ucsi_host.cpp).
//   witnesses  UCSI's GET_CONNECTOR_STATUS (UcsiClient.h: connected, partner type, power direction, the 2.x orientation bit), the
//              DSP's port notification (owner 0x800c: orientation and mux state), gpio61 (pmic-glink's orientation line, read at each
//              status as ucsi_glink reads it: high = reverse, meaningful only with a cable in), the PMIC's Type-C block when SPMI is
//              ours (not with PianoPower running: one observer channel serves one reader). The role is decideRole()'s
//              (UsbHostSurvey.h): UCSI's partner type first, a UFP (or a powered cable + UFP) -> host, a DFP -> device, nothing ->
//              device (the debug cable always finds PianoDwc3 armed).
//   switch     default off (piano-usbhost-handoff=0: the decision is published and logged as "would switch", nothing is written).
//              On: host once the decision has read host for the debounce (1 s: a dock's data-role swap settles first), device at
//              once when it reads anything else (an unplug gives the port back to the debug cable without delay); a failed host
//              attempt is not repeated until the partner goes away.
#pragma once
#include "UsbHostSurvey.h"
#include "Ucsi.h"

namespace pianousbhost {

struct PortWitnesses {
  bool ucsiReady = false;            // the UCSI client is ready and its status valid
  uint16_t ucsiVersion = 0;
  pianoucsi::ConnectorStatus ucsi;
  bool panSeen = false;              // a port notification for port 0 since the generation began
  pianoucsi::PortNotification pan;
  bool gpioRead = false, gpioHigh = false;
  bool typecValid = false;
  TypecView typec;
};

struct PortView {
  RoleDecision role;
  bool known = false;                // some witness says whether a cable is in
  bool connected = false;
  int vbus = -1;                     // 1 we source VBUS (UCSI: provider; PMIC: source mode), 0 we sink, -1 unknown
  unsigned ucsiO = 0, panO = 0, gpioO = 0, pmicO = 0;   // each witness: 0 unknown, 1 normal, 2 reverse
  unsigned orientation = 0;          // the first known of UCSI (2.x), the port notification, gpio61, the PMIC
  bool orientationKnown = false, agree = true;
};

inline unsigned witness(int o) { return o == 0 ? 1 : o == 1 ? 2 : 0; }

inline PortView portView(const PortWitnesses &w)
{
  PortView v;
  RoleInput in;
  in.ucsiValid = w.ucsiReady && w.ucsi.valid;
  in.ucsiConnected = w.ucsi.connected;
  in.ucsiPartner = w.ucsi.partnerType;
  in.typecValid = w.typecValid;
  in.typec = w.typec;
  v.role = decideRole(in);
  if (in.ucsiValid) {
    v.known = true;
    v.connected = w.ucsi.connected;
    if (v.connected) v.vbus = w.ucsi.provider ? 1 : 0;
  } else if (w.typecValid) {
    v.known = true;
    v.connected = w.typec.attached;
    if (v.connected) v.vbus = w.typec.sourceMode ? 1 : 0;
  } else if (w.panSeen) {
    v.known = true;
    v.connected = w.pan.mux != pianoucsi::kMuxNone;
  }
  if (!v.connected) return v;
  v.ucsiO = in.ucsiValid ? witness(w.ucsi.orientation) : 0;
  v.panO = w.panSeen && w.pan.mux != pianoucsi::kMuxNone ? witness(pianoucsi::portOrientation(w.pan.orientation)) : 0;
  v.gpioO = w.gpioRead ? (w.gpioHigh ? 2 : 1) : 0;
  v.pmicO = w.typecValid && w.typec.attached ? w.typec.cc : 0;
  const unsigned seen[4] = {v.ucsiO, v.panO, v.gpioO, v.pmicO};
  for (unsigned s : seen) {
    if (!s) continue;
    if (!v.orientation) v.orientation = s;
    else if (s != v.orientation) v.agree = false;
  }
  v.orientationKnown = v.orientation != 0;
  return v;
}

// ------------------------------------------------------------------------------------------------------------ the switch
enum class SwitchAction : uint8_t { none, toHost, toDevice };
inline const char *actionName(SwitchAction a)
{
  return a == SwitchAction::toHost ? "to host" : a == SwitchAction::toDevice ? "to device" : "none";
}

struct RoleSwitch {
  bool enabled = false;
  uint32_t debounceMs = 1000;
  Role current = Role::device;       // the controller's role as we left it (device: PianoDwc3's or the firmware's)
  Role wanted = Role::device;
  uint64_t wantedSince = 0;
  bool failedThisConnection = false; // a host attempt failed: not again until the partner goes away
  bool deferred = false;             // the device side asked us to wait (PianoDwc3 still starting): retry at deferUntil
  uint64_t deferUntil = 0;
  uint32_t toHost = 0, toDevice = 0, failures = 0, wouldSwitch = 0;

  // The decision of this moment (none: device mode stays armed, as device), whether a cable is in. Returns what to do now.
  SwitchAction step(Role decided, bool connected, uint64_t now)
  {
    const Role want = decided == Role::host ? Role::host : Role::device;
    if (want != wanted) {
      wanted = want;
      wantedSince = now;
      if (want == Role::host && current == Role::device && !enabled) wouldSwitch++;
    }
    if (!connected) failedThisConnection = false;
    if (current == Role::host && wanted != Role::host) return SwitchAction::toDevice;
    if (current == Role::device && wanted == Role::host && enabled && !failedThisConnection && now - wantedSince >= debounceMs &&
        (!deferred || now >= deferUntil))
      return SwitchAction::toHost;
    return SwitchAction::none;
  }
  // The action's outcome. A host attempt that failed leaves the controller in device mode (the kext returned it).
  void done(SwitchAction a, bool ok)
  {
    deferred = false;
    if (a == SwitchAction::toHost) {
      if (ok) {
        current = Role::host;
        toHost++;
      } else {
        failedThisConnection = true;
        failures++;
      }
    } else if (a == SwitchAction::toDevice) {
      current = Role::device;
      toDevice++;
      if (!ok) failures++;
    }
  }
  // The device side is not ready to hand over yet: ask again later.
  void defer(uint64_t until)
  {
    deferred = true;
    deferUntil = until;
  }
  // When step() must run again on its own (the debounce's end, a deferred retry); 0 none.
  uint64_t deadline() const
  {
    if (!enabled || current != Role::device || wanted != Role::host || failedThisConnection) return 0;
    const uint64_t t = wantedSince + debounceMs;
    return deferred && deferUntil > t ? deferUntil : t;
  }
};

}  // namespace pianousbhost
