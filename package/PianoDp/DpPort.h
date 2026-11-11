// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDp D2: the DSP's USB-C port notifications (PMIC GLINK owner 0x800c, opcode 0x16) as PianoPower hands them over
// (PianoAdspContract.h, piano_port_events): the decoder, the pin-assignment table, the link budget and the per-port HPD state machine
// whose transitions the kext logs (attach, detach, DisplayPort entry and exit, pin assignment, orientation, HPD up/down, IRQ_HPD).
// Portable, no I/O and no allocation (host tests in tests/dp_host.cpp). Nothing here sends anything: the acknowledgement after every
// notification is PianoPower's (PianoMca.c), and the switch, PHY and controller writes a plug would need are levels >= 2 (docs
// section 6); the "plan" below only names them for the log. Facts: DpHw.h (glink, fsa, qmp, DT).
#pragma once
#include <stdint.h>
#include "DpHw.h"

namespace pianodp {

// ------------------------------------------------------------------------------------------------------------ the decoder
struct PortEvent {
  bool valid = false;
  const char *why = "";        // why not valid
  uint8_t port = 0, orientation = 0, mux = 0, pin = 0;   // orientation as sent (0 normal, 1 reverse, else none); pin 0..6 (0 none)
  uint16_t svid = 0, vid = 0, svidPayload = 0;           // svid: the header's (what Linux uses); svidPayload: the copy at @18
  bool dp = false;             // SVID DisplayPort
  bool hpd = false, irq = false;
  uint8_t ext = 0;             // the extended data byte @20 as sent
};
inline uint32_t le32At(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline PortEvent decodePortEvent(const uint8_t *d, uint32_t length)
{
  PortEvent e;
  if (!d || length < 12) {
    e.why = "shorter than the GLINK header";
    return e;
  }
  const uint32_t owner = le32At(d), opcode = le32At(d + 8);
  if (owner != kPanOwner || (opcode & 0xffu) != kPanNotify) {
    e.why = "not a port notification (owner 0x800c, opcode 0x16)";
    return e;
  }
  if (length != kNotifyBytes) {   // pmic_glink_altmode_sc8280xp_notify takes exactly sizeof(struct usbc_notify)
    e.why = "not 32 bytes (Linux drops it)";
    return e;
  }
  e.port = d[12];
  e.orientation = d[13];
  e.mux = d[14];
  e.vid = uint16_t(d[16] | d[17] << 8);
  e.svidPayload = uint16_t(d[18] | d[19] << 8);
  e.svid = uint16_t(opcode >> 16);
  e.ext = d[20];
  if (e.port >= kPorts) {
    e.why = "port index above 2";
    return e;
  }
  e.dp = e.svid == kSvidDp;
  if (e.dp) {
    e.pin = e.ext & 0x3fu;
    e.hpd = (e.ext >> 6) & 1u;
    e.irq = (e.ext >> 7) & 1u;
  }
  e.valid = true;
  return e;
}
inline const char *orientationName(uint8_t o) { return o == 0 ? "normal" : o == 1 ? "reverse" : "none"; }
inline const char *muxName(uint8_t m)
{
  switch (m) {
    case kMuxNone: return "no connection";
    case kMuxUsb3: return "USB3 only";
    case kMuxDp4: return "DP 4 lanes";
    case kMuxUsb3Dp: return "USB3 + DP";
    case kMuxTunnel: return "tunnelling (USB4/TBT)";
    default: return "unknown";
  }
}
inline const char *svidName(uint16_t s) { return s == kSvidDp ? "DisplayPort" : s == kSvidTbt ? "Thunderbolt" : s == 0 ? "none" : "other"; }

// ------------------------------------------------------------------------------------------------- the pin assignments
// Lanes and USB per pin (VESA DP Alt Mode; what msm and fsa4480 do with each): C and E carry DP on all four lanes (no USB 3: USB 2
// only), D and F two DP lanes beside USB 3. A and B are the legacy assignments: fsa4480_set refuses them and msm's QMP mux takes them as
// its USB3 + DP fallback, so they are not driven here either. The QMP mode word is DP_COM PHY_MODE_CTRL's (USB3 bit 0, DP bit 1).
struct PinInfo {
  char letter;
  uint8_t lanes;
  bool usb3, supported;
  uint8_t qmpMode;                       // the DP_COM mode a plug would set: 2 DP only, 3 USB3 + DP (0: none)
  uint8_t dioEnable, dioSelect;          // the DIO4485's ENABLE and SELECT for the normal orientation (reverse: | SBU reverse)
};
inline PinInfo pinInfo(uint8_t pin)
{
  switch (pin) {
    case 1: return {'A', 4, false, false, 0, 0, 0};
    case 2: return {'B', 2, true, false, 0, 0, 0};
    case 3: return {'C', 4, false, true, 2, uint8_t(kDioDevice | kDioSbu), 0};
    case 4: return {'D', 2, true, true, 3, uint8_t(kDioDevice | kDioUsb | kDioSbu), kDioSelUsb};
    case 5: return {'E', 4, false, true, 2, uint8_t(kDioDevice | kDioSbu), 0};
    case 6: return {'F', 2, true, true, 3, uint8_t(kDioDevice | kDioUsb | kDioSbu), kDioSelUsb};
    default: return {'-', 0, true, false, 0, 0, 0};
  }
}
inline uint8_t dioSelect(const PinInfo &p, bool reverse) { return uint8_t(p.dioSelect | (reverse ? kDioSelSbuReverse : 0)); }
// The USB-only state (fsa4480_set's USB mode): ENABLE DEVICE|USB, SELECT USB.
constexpr uint8_t kDioUsbEnable = kDioDevice | kDioUsb, kDioUsbSelect = kDioSelUsb;

// --------------------------------------------------------------------------------------------------------- the link budget
// 8b/10b: payload = lanes x rate x 8 / 10. A mode fits when pixel clock x bits per pixel stays within it (the blanking is in the pixel
// clock; MSA/TU overhead aside, which the stream level computes).
constexpr uint64_t payloadMbps(unsigned lanes, unsigned rateMbps) { return uint64_t(lanes) * rateMbps * 8 / 10; }
constexpr bool modeFits(uint32_t pixelKhz, unsigned bpp, unsigned lanes, unsigned rateMbps)
{
  return uint64_t(pixelKhz) * bpp <= payloadMbps(lanes, rateMbps) * 1000;
}
struct StdMode {
  const char *name;
  uint32_t pixelKhz;   // CTA-861 or CVT reduced blanking
};
// The modes the log names (4K60 CVT-RB 533.25 MHz as displayport.md section 4.3; 4K30 and 1080p60 CTA-861; 1440p60 CVT-RB).
constexpr StdMode kStdModes[] = {{"3840x2160@60", 533250}, {"2560x1440@60", 241500}, {"3840x2160@30", 297000}, {"1920x1080@60", 148500}};
constexpr unsigned kStdModeCount = sizeof kStdModes / sizeof kStdModes[0];
// The standard modes that fit at 8 bpc RGB (24 bpp) on LANES at RATE (default piano's HBR2 cap): bit i = kStdModes[i].
inline unsigned fittingModes(unsigned lanes, unsigned rateMbps = kRateMaxMbps)
{
  unsigned bits = 0;
  for (unsigned i = 0; i < kStdModeCount; ++i)
    if (modeFits(kStdModes[i].pixelKhz, 24, lanes, rateMbps)) bits |= 1u << i;
  return bits;
}

// ------------------------------------------------------------------------------------------------- the per-port state
struct PortState {
  bool seen = false;           // a valid notification arrived for this port
  bool attached = false;       // mux != no connection
  bool dp = false;             // in DisplayPort alt mode with a pin assignment (C..F, or A/B logged as unsupported)
  bool hpd = false;
  uint8_t mux = 0, orientation = 0xff, pin = 0;
  uint16_t svid = 0;
  uint32_t events = 0, irqs = 0, plugs = 0, unplugs = 0;
};
// What one notification changed (a bit set; the kext logs each).
enum : uint32_t {
  kChAttach = 1u << 0,         // mux from no connection to something
  kChDetach = 1u << 1,         // mux to no connection
  kChDpEnter = 1u << 2,        // DisplayPort alt mode entered (SVID DP with a connection and a pin assignment)
  kChDpExit = 1u << 3,         // left (SVID changed, the safe state, or detached)
  kChPin = 1u << 4,            // the pin assignment changed while in DP
  kChOrientation = 1u << 5,
  kChMux = 1u << 6,            // mux changed while attached (e.g. USB3 only -> USB3 + DP)
  kChHpdUp = 1u << 7,          // HPD high: a sink is there: plug (D3+: DPCD, EDID, training)
  kChHpdDown = 1u << 8,        // HPD low: unplug (the stream parked, the PHY down)
  kChIrqHpd = 1u << 9,         // IRQ_HPD while HPD high: the sink asks for attention (D3+: DPCD 0x200-0x205)
  kChIrqLow = 1u << 10,        // IRQ_HPD with HPD low (not a valid DP event: logged)
  kChUnsupportedPin = 1u << 11,// pin A, B or none in DP: not driven
  kChPinMuxMismatch = 1u << 12,// the pin's lane count does not match the mux (C/E with USB3 + DP, D/F with DP 4 lanes)
  kChRepeat = 1u << 13,        // nothing changed (a repeat, e.g. after the firmware's 13 s re-send)
};
struct PortStep {
  uint32_t changes = 0;
  PortState before, after;
};
// One notification into the port's state (Linux's worker order: orientation, then by SVID and mux, HPD with SVID DP).
inline PortStep portStep(PortState &s, const PortEvent &e)
{
  PortStep r;
  r.before = s;
  const bool attached = e.mux != kMuxNone;
  const bool dpMode = e.dp && attached;
  const PinInfo pin = pinInfo(e.pin);
  s.seen = true;
  s.events++;
  if (attached && !r.before.attached) r.changes |= kChAttach;
  if (!attached && r.before.attached) r.changes |= kChDetach;
  if (attached && r.before.attached && e.mux != r.before.mux) r.changes |= kChMux;
  if (r.before.seen && e.orientation != r.before.orientation && attached) r.changes |= kChOrientation;
  if (dpMode && !r.before.dp) r.changes |= kChDpEnter;
  if (!dpMode && r.before.dp) r.changes |= kChDpExit;
  if (dpMode && r.before.dp && e.pin != r.before.pin) r.changes |= kChPin;
  if (dpMode && !pin.supported) r.changes |= kChUnsupportedPin;
  if (dpMode && pin.supported && ((pin.lanes == 4) != (e.mux == kMuxDp4))) r.changes |= kChPinMuxMismatch;
  // HPD: reported with SVID DP, also in the safe state (SVID DP, no connection: an HPD-low report, Linux's special case).
  const bool hpd = e.dp && e.hpd;
  if (hpd && !r.before.hpd) r.changes |= kChHpdUp;
  if (!hpd && r.before.hpd) r.changes |= kChHpdDown;
  if (e.dp && e.irq) r.changes |= hpd ? kChIrqHpd : kChIrqLow;
  s.attached = attached;
  s.dp = dpMode;
  s.hpd = hpd;
  s.mux = e.mux;
  s.orientation = e.orientation;
  s.pin = e.dp ? e.pin : 0;
  s.svid = e.svid;
  if (r.changes & kChHpdUp) s.plugs++;
  if (r.changes & kChHpdDown) s.unplugs++;
  if (r.changes & kChIrqHpd) s.irqs++;
  if (!(r.changes & ~(kChUnsupportedPin | kChPinMuxMismatch))) r.changes |= kChRepeat;
  r.after = s;
  return r;
}

// ------------------------------------------------------------------------------------------- what a plug would program
// For the log only (nothing is written at level 1): the DIO4485 words, the QMP mode, the lanes and what they carry at HBR2.
struct PlugPlan {
  bool drive = false;          // a supported pin in DP mode
  uint8_t dioEnable = kDioUsbEnable, dioSelect = kDioUsbSelect, qmpMode = 1, lanes = 0;
  bool usb3 = true;
  unsigned modes = 0;          // fittingModes(lanes): the standard modes these lanes carry at HBR2, 8 bpc
};
inline PlugPlan plugPlan(const PortState &s)
{
  PlugPlan p;
  if (!s.dp) return p;
  const PinInfo pin = pinInfo(s.pin);
  if (!pin.supported) return p;
  p.drive = true;
  p.dioEnable = pin.dioEnable;
  p.dioSelect = dioSelect(pin, s.orientation == 1);
  p.qmpMode = pin.qmpMode;
  p.lanes = pin.lanes;
  p.usb3 = pin.usb3;
  p.modes = fittingModes(pin.lanes);
  return p;
}

}  // namespace pianodp
