// SPDX-License-Identifier: BSD-2-Clause-Patent
// The USB Type-C Connector System Software Interface (UCSI) as piano's DSP serves it on PianoPower's PMIC_RTR_ADSP_APPS channel (owner
// 0x800b, package/include/PianoUsbcContract.h), and the DSP's USB-C port notifications (owner 0x800c): the framing, the mailbox, the
// commands and the data structures the client (UcsiClient.h) needs for roles, power direction and orientation. Portable, no I/O.
// Specification: the UCSI specification (Intel, revisions 1.2 and 2.0: the mailbox layout, CCI, CONTROL, the command codes and the
// GET_CAPABILITY / GET_CONNECTOR_CAPABILITY / GET_CONNECTOR_STATUS / GET_ERROR_STATUS data structures), and as described behaviour
// Linux drivers/usb/typec/ucsi/{ucsi.h,ucsi.c,ucsi_glink.c} and drivers/soc/qcom/pmic_glink_altmode.c (GPL-2.0, read, nothing copied);
// QcomDwc3's QcomUcsi.h (BSD-2-Clause-Patent) for the GLINK framing it already wrote down. Names and code are ours.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pianoucsi {

// ---------------------------------------------------------------------------------------------------------- GLINK framing
// Every message: {owner, type, opcode}, three little-endian words. A read request is the header alone; its answer the header, the
// mailbox (48 bytes for UCSI 1.x, 528 for 2.x) and a return code. A write request: the header, a whole mailbox image (the command in
// CONTROL) and a reserved word; its answer the header and a return code. A notification (read the CCI now): the header and three
// words (notification, receiver, reserved).
constexpr uint32_t kOwner = 0x800b, kOwnerPort = 0x800c, kTypeRequest = 1;
constexpr uint32_t kOpRead = 0x11, kOpWrite = 0x12, kOpNotify = 0x13, kOpPortNotify = 0x16;
constexpr uint32_t kHeader = 12, kBufferV1 = 48, kBufferV2 = 528, kMaxRequest = kHeader + kBufferV2 + 4;

inline void put32(uint8_t *p, uint32_t v)
{
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}
inline uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }

inline uint32_t readRequest(uint8_t *out)
{
  put32(out, kOwner);
  put32(out + 4, kTypeRequest);
  put32(out + 8, kOpRead);
  return kHeader;
}
// The command in CONTROL (offset 8 of the mailbox), the rest of the image zero.
inline uint32_t writeRequest(uint8_t *out, uint32_t bufferBytes, uint64_t control)
{
  const uint32_t length = kHeader + bufferBytes + 4;
  for (uint32_t i = 0; i < length; ++i) out[i] = 0;
  put32(out, kOwner);
  put32(out + 4, kTypeRequest);
  put32(out + 8, kOpWrite);
  put32(out + kHeader + 8, uint32_t(control));
  put32(out + kHeader + 12, uint32_t(control >> 32));
  return length;
}

// ------------------------------------------------------------------------------------------------------------- the mailbox
constexpr uint32_t kMbVersion = 0, kMbCci = 4, kMbControl = 8, kMbMessageIn = 16;
constexpr uint16_t kV1_0 = 0x0100, kV1_1 = 0x0110, kV1_2 = 0x0120, kV2_0 = 0x0200;
// The mailbox a version uses (UCSI 2.0 grew MESSAGE_IN and MESSAGE_OUT to 256 bytes).
inline uint32_t bufferFor(uint16_t version) { return version >= kV2_0 ? kBufferV2 : kBufferV1; }
inline uint32_t messageInBytes(uint16_t version) { return version >= kV2_0 ? 256 : 16; }

// CCI: bits 7:1 a connector with a change to report, 15:8 the length of MESSAGE_IN's data, then the indicators.
constexpr uint32_t kCciNotSupported = 1u << 25, kCciCancelComplete = 1u << 26, kCciResetComplete = 1u << 27, kCciBusy = 1u << 28,
                   kCciAckComplete = 1u << 29, kCciError = 1u << 30, kCciCommandComplete = 1u << 31;
inline unsigned cciConnector(uint32_t cci) { return (cci >> 1) & 0x7fu; }
inline unsigned cciLength(uint32_t cci) { return (cci >> 8) & 0xffu; }

// ------------------------------------------------------------------------------------------------------------- commands
// CONTROL: the command code in bits 7:0, its data length in 15:8 (0 here), command-specific bits from 16.
enum Command : uint8_t {
  kPpmReset = 0x01, kCancel = 0x02, kConnectorReset = 0x03, kAckCcCi = 0x04, kSetNotificationEnable = 0x05, kGetCapability = 0x06,
  kGetConnectorCapability = 0x07, kSetUom = 0x08, kSetUor = 0x09, kSetPdm = 0x0a, kSetPdr = 0x0b, kGetAlternateModes = 0x0c,
  kGetCamSupported = 0x0d, kGetCurrentCam = 0x0e, kSetNewCam = 0x0f, kGetPdos = 0x10, kGetCableProperty = 0x11,
  kGetConnectorStatus = 0x12, kGetErrorStatus = 0x13,
};
inline uint8_t commandOf(uint64_t control) { return uint8_t(control & 0xffu); }
inline uint64_t connectorNumber(unsigned n) { return uint64_t(n & 0x7fu) << 16; }
// ACK_CC_CI
constexpr uint64_t kAckConnectorChange = 1ull << 16, kAckCommandComplete = 1ull << 17;
// SET_NOTIFICATION_ENABLE (the bits of CONTROL)
constexpr uint64_t kNtfyCommandComplete = 1ull << 16, kNtfyExternalSupply = 1ull << 17, kNtfyPowerMode = 1ull << 18,
                   kNtfyAttention = 1ull << 19, kNtfyFirmwareUpdate = 1ull << 20, kNtfyCapabilities = 1ull << 21,
                   kNtfyPowerLevel = 1ull << 22, kNtfyPdReset = 1ull << 23, kNtfyCam = 1ull << 24, kNtfyBattery = 1ull << 25,
                   kNtfySecurity = 1ull << 26, kNtfyPartner = 1ull << 27, kNtfyPowerDirection = 1ull << 28, kNtfyRetimer = 1ull << 29,
                   kNtfyConnect = 1ull << 30, kNtfyError = 1ull << 31, kNtfySinkPath = 1ull << 32;
constexpr uint64_t kNtfyAll = 0xdbe70000ull;   // every 1.x notification (bits 16-18, 21-25, 27, 28, 30, 31)
// SET_UOR: the connector, then DFP (we host) or UFP (we are the device), and whether role swaps are accepted afterwards.
constexpr uint64_t kUorDfp = 1ull << 23, kUorUfp = 1ull << 24, kUorAcceptSwaps = 1ull << 25;
// SET_PDR: the connector, then provider (we source) or consumer (we sink), and whether power swaps are accepted afterwards.
constexpr uint64_t kPdrProvider = 1ull << 23, kPdrConsumer = 1ull << 24, kPdrAcceptSwaps = 1ull << 25;
inline uint64_t setUor(unsigned connector, bool host, bool acceptSwaps)
{
  return kSetUor | connectorNumber(connector) | (host ? kUorDfp : kUorUfp) | (acceptSwaps ? kUorAcceptSwaps : 0);
}
inline uint64_t setPdr(unsigned connector, bool source, bool acceptSwaps)
{
  return kSetPdr | connectorNumber(connector) | (source ? kPdrProvider : kPdrConsumer) | (acceptSwaps ? kPdrAcceptSwaps : 0);
}

// ------------------------------------------------------------------------------------------------------------ GET_CAPABILITY
// bmAttributes (4 bytes), bNumConnectors (bits 6:0), bmOptionalFeatures (2 bytes used), a reserved byte, bNumAltModes, a reserved
// byte, bcdBCVersion, bcdPDVersion, bcdUSBTypeCVersion.
constexpr uint32_t kAttrDisabledState = 1u << 0, kAttrBatteryCharging = 1u << 1, kAttrUsbPd = 1u << 2, kAttrTypecCurrent = 1u << 6;
constexpr uint16_t kFeatSetUom = 1u << 0, kFeatSetPdm = 1u << 1, kFeatAltModeDetails = 1u << 2, kFeatAltModeOverride = 1u << 3,
                   kFeatPdoDetails = 1u << 4, kFeatCableDetails = 1u << 5, kFeatExtSupplyNotifications = 1u << 6, kFeatPdReset = 1u << 7,
                   kFeatGetPdMessage = 1u << 8, kFeatAttentionVdo = 1u << 9, kFeatFirmwareUpdate = 1u << 10, kFeatSecurity = 1u << 12,
                   kFeatRetimer = 1u << 13;
struct Capability {
  bool valid = false;
  uint32_t attributes = 0;
  uint8_t connectors = 0, altModes = 0;
  bool reservedBit = false;   // bNumConnectors bit 7 set (out of the specification; Linux clears it and carries on)
  uint16_t features = 0, bcVersion = 0, pdVersion = 0, typecVersion = 0;
};
inline Capability decodeCapability(const uint8_t *m)
{
  Capability c;
  c.valid = true;
  c.attributes = get32(m);
  c.reservedBit = (m[4] & 0x80u) != 0;
  c.connectors = m[4] & 0x7fu;
  c.features = get16(m + 5);
  c.altModes = m[8];
  c.bcVersion = get16(m + 10);
  c.pdVersion = get16(m + 12);
  c.typecVersion = get16(m + 14);
  return c;
}
// The notifications to enable once the connectors are known (Linux's ucsi_get_supported_notifications as described behaviour): all of
// 1.x but those the optional features say cannot come; from 2.0 the sink path status and the optional 2.x ones.
inline uint64_t supportedNotifications(uint16_t version, uint16_t features)
{
  uint64_t n = kNtfyAll;
  if (!(features & kFeatAltModeDetails)) n &= ~kNtfyCam;
  if (!(features & kFeatPdoDetails)) n &= ~(kNtfyPowerLevel | kNtfyCapabilities);
  if (!(features & kFeatExtSupplyNotifications)) n &= ~kNtfyExternalSupply;
  if (!(features & kFeatPdReset)) n &= ~kNtfyPdReset;
  if (version <= kV1_2) return n;
  n |= kNtfySinkPath;
  if (features & kFeatAttentionVdo) n |= kNtfyAttention;
  if (features & kFeatFirmwareUpdate) n |= kNtfyFirmwareUpdate;
  if (features & kFeatSecurity) n |= kNtfySecurity;
  if (features & kFeatRetimer) n |= kNtfyRetimer;
  return n;
}

// --------------------------------------------------------------------------------------------- GET_CONNECTOR_CAPABILITY
// The operation modes (bits 7:0), provider (8), consumer (9), and from 1.1 the role swaps the port supports (10..13).
constexpr uint16_t kOpDfp = 1u << 0, kOpUfp = 1u << 1, kOpDrp = 1u << 2, kOpAudio = 1u << 3, kOpDebug = 1u << 4, kOpUsb2 = 1u << 5,
                   kOpUsb3 = 1u << 6, kOpAltMode = 1u << 7, kCapProvider = 1u << 8, kCapConsumer = 1u << 9, kCapSwapToDfp = 1u << 10,
                   kCapSwapToUfp = 1u << 11, kCapSwapToSrc = 1u << 12, kCapSwapToSnk = 1u << 13;
struct ConnectorCapability {
  bool valid = false;
  uint16_t bits = 0;
};
inline ConnectorCapability decodeConnectorCapability(const uint8_t *m)
{
  ConnectorCapability c;
  c.valid = true;
  c.bits = get16(m);
  return c;
}

// ----------------------------------------------------------------------------------------------- GET_CONNECTOR_STATUS
// Bits 15:0 the changes since the last acknowledgement, 18:16 the power operation mode, 19 connected, 20 power direction (1: we are
// the provider: we source VBUS), 28:21 the partner flags (21 USB, 22 alternate mode), 31:29 the partner type, 63:32 the request data
// object, 65:64 the battery charging status; 1.2: 85:70 the partner's PD revision; 2.0: 86 orientation (1 reverse), 87 sink path.
constexpr uint16_t kChangeExternalSupply = 1u << 1, kChangePowerMode = 1u << 2, kChangePdos = 1u << 5, kChangePowerLevel = 1u << 6,
                   kChangePdReset = 1u << 7, kChangeCam = 1u << 8, kChangeBattery = 1u << 9, kChangePartner = 1u << 11,
                   kChangePowerDirection = 1u << 12, kChangeSinkPath = 1u << 13, kChangeConnect = 1u << 14, kChangeError = 1u << 15;
enum PowerMode : uint8_t { kModeNone = 0, kModeDefault = 1, kModeBc = 2, kModePd = 3, kModeTypec1_5 = 4, kModeTypec3_0 = 5 };
enum PartnerType : uint8_t { kPartnerNone = 0, kPartnerDfp = 1, kPartnerUfp = 2, kPartnerCable = 3, kPartnerCableUfp = 4,
                             kPartnerDebug = 5, kPartnerAudio = 6 };
constexpr uint8_t kPartnerFlagUsb = 1u << 0, kPartnerFlagAltMode = 1u << 1;
struct ConnectorStatus {
  bool valid = false;
  uint16_t change = 0;
  uint8_t powerMode = 0, partnerFlags = 0, partnerType = 0, battery = 0;
  bool connected = false, provider = false;
  uint32_t rdo = 0;
  int orientation = -1;      // UCSI 2.0 and later only: 0 normal, 1 reverse; -1 not reported (1.x)
  uint8_t raw[16] = {};
};
// A bit field of the little-endian structure in m (bytes long).
inline uint32_t bits(const uint8_t *m, unsigned bytes, unsigned offset, unsigned size)
{
  uint32_t v = 0;
  for (unsigned i = 0; i < size; ++i) {
    const unsigned b = offset + i;
    if (b / 8 < bytes && (m[b / 8] >> (b % 8)) & 1u) v |= 1u << i;
  }
  return v;
}
inline ConnectorStatus decodeStatus(const uint8_t *m, unsigned bytes, uint16_t version)
{
  ConnectorStatus s;
  s.valid = true;
  s.change = uint16_t(bits(m, bytes, 0, 16));
  s.powerMode = uint8_t(bits(m, bytes, 16, 3));
  s.connected = bits(m, bytes, 19, 1) != 0;
  s.provider = bits(m, bytes, 20, 1) != 0;
  s.partnerFlags = uint8_t(bits(m, bytes, 21, 8));
  s.partnerType = uint8_t(bits(m, bytes, 29, 3));
  s.rdo = bits(m, bytes, 32, 32);
  s.battery = uint8_t(bits(m, bytes, 64, 2));
  if (version >= kV2_0 && bytes >= 11) s.orientation = int(bits(m, bytes, 86, 1));
  for (unsigned i = 0; i < 16 && i < bytes; ++i) s.raw[i] = m[i];
  return s;
}
inline const char *partnerTypeName(uint8_t t)
{
  switch (t) {
    case kPartnerDfp: return "DFP";
    case kPartnerUfp: return "UFP";
    case kPartnerCable: return "powered cable";
    case kPartnerCableUfp: return "powered cable + UFP";
    case kPartnerDebug: return "debug accessory";
    case kPartnerAudio: return "audio accessory";
    default: return "none";
  }
}
inline const char *powerModeName(uint8_t m)
{
  switch (m) {
    case kModeDefault: return "USB default";
    case kModeBc: return "BC 1.2";
    case kModePd: return "PD";
    case kModeTypec1_5: return "Type-C 1.5 A";
    case kModeTypec3_0: return "Type-C 3.0 A";
    default: return "none";
  }
}
inline const char *commandName(uint8_t c)
{
  switch (c) {
    case kPpmReset: return "PPM_RESET";
    case kCancel: return "CANCEL";
    case kConnectorReset: return "CONNECTOR_RESET";
    case kAckCcCi: return "ACK_CC_CI";
    case kSetNotificationEnable: return "SET_NOTIFICATION_ENABLE";
    case kGetCapability: return "GET_CAPABILITY";
    case kGetConnectorCapability: return "GET_CONNECTOR_CAPABILITY";
    case kSetUor: return "SET_UOR";
    case kSetPdr: return "SET_PDR";
    case kGetConnectorStatus: return "GET_CONNECTOR_STATUS";
    case kGetErrorStatus: return "GET_ERROR_STATUS";
    default: return "command";
  }
}
// GET_ERROR_STATUS: bmErrorInformation (16 bits).
inline const char *errorName(uint16_t e)
{
  if (e & (1u << 0)) return "unrecognized command";
  if (e & (1u << 1)) return "invalid connector number";
  if (e & (1u << 2)) return "invalid command argument";
  if (e & (1u << 3)) return "incompatible partner";
  if (e & (1u << 4)) return "CC communication error";
  if (e & (1u << 5)) return "dead battery";
  if (e & (1u << 6)) return "contract negotiation failure";
  if (e & (1u << 7)) return "overcurrent";
  if (e & (1u << 9)) return "partner rejected swap";
  if (e & (1u << 10)) return "hard reset";
  if (e & (1u << 11)) return "PPM policy conflict";
  if (e & (1u << 12)) return "swap rejected";
  return e ? "undefined" : "none";
}

// ------------------------------------------------------------------------------------------------- the port notification
// Owner 0x800c, opcode 0x16 in the low byte, the SVID in bits 31:16; then the port, the orientation (0 normal, 1 reverse, else none),
// the mux state, a reserved byte, the VID and SVID (16 bits each), 8 bytes of DP (pin assignment 5:0, HPD state 6, HPD IRQ 7) or TBT
// data, and a reserved word: 32 bytes (Linux checks that length; the first three bytes are read from any notification of 15 or more).
enum Mux : uint8_t { kMuxNone = 0, kMuxUsb3 = 1, kMuxDp4 = 2, kMuxUsb3Dp = 3, kMuxTunnel = 4 };
constexpr uint16_t kSvidDp = 0xff01, kSvidTbt = 0x8087;
struct PortNotification {
  bool ok = false, full = false;
  uint8_t port = 0, orientation = 0, mux = 0, dp = 0;
  uint16_t svid = 0, vid = 0;
};
inline PortNotification decodePort(const uint8_t *d, uint32_t length)
{
  PortNotification p;
  if (!d || length < 15 || get32(d) != kOwnerPort || (get32(d + 8) & 0xffu) != kOpPortNotify) return p;
  p.ok = true;
  p.svid = uint16_t(get32(d + 8) >> 16);
  p.port = d[12];
  p.orientation = d[13];
  p.mux = d[14];
  if (length >= 32) {
    p.full = true;
    p.vid = get16(d + 16);
    p.dp = d[20];
  }
  return p;
}
inline const char *muxName(uint8_t m)
{
  switch (m) {
    case kMuxNone: return "no connection";
    case kMuxUsb3: return "USB3 only";
    case kMuxDp4: return "DP 4 lanes";
    case kMuxUsb3Dp: return "USB3 + DP 2 lanes";
    case kMuxTunnel: return "tunnelling";
    default: return "unknown";
  }
}
// 0 normal, 1 reverse, -1 none (any other value)
inline int portOrientation(uint8_t o) { return o == 0 ? 0 : o == 1 ? 1 : -1; }

}  // namespace pianoucsi
