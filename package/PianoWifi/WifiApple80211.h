// SPDX-License-Identifier: BSD-2-Clause-Patent
// The apple80211 requests (docs/drivers/wifi-menu.md section 5, milestone M1): the BSD ioctls SIOCSA80211 / SIOCGA80211 that
// airportd and IO80211.framework send to a Wi-Fi interface (private/host-checks/20261114-io80211/REPORT.md section 2: the 40-byte
// request, the 27.3 request numbers), their answers' layouts and the handler that answers M1's set from PianoWifi's own state:
// the identity, the power, the channels, the scan from level 6/7's scan list, and a consistent "not associated" for everything
// a join would fill. Portable: no kernel header; the kext (PianoWifi80211.h) gives it copyin/copyout, a scratch buffer and its
// lock, and does what the answer asks for (events, the scan's completion). Host tests: tests/apple80211_host.cpp.
//
// Prior art: UniXNU-sheng package/WifiNet.cpp (our own organisation's, BSD-2-Clause-Patent, UPSTREAM), a raw SIOC[SG]A80211
// binding for iPadOS 24A446's wifid. Its layouts are used here where the 27.3 host analysis agrees and marked where it does not
// say (each struct below says which). Nothing of OpenIntelWireless (GPL) is used.
#pragma once
#include <stdint.h>
#include "WifiHw.h"   // kScanEntriesMax, kSsidMax

namespace pianowifi {
namespace a80211 {

// ---------------------------------------------------------------------------------------------------------- the transport
// _IOW('i', 200, struct apple80211req) and _IOWR('i', 201, ...): 27.3 confirmed (REPORT.md 2.1).
constexpr unsigned long kSiocSA80211 = 0x802869c8UL;
constexpr unsigned long kSiocGA80211 = 0xc02869c9UL;

// The request as the ioctl carries it (64-bit user space): 27.3 confirmed, 40 bytes.
struct Req {
  char name[16];      // the BSD interface ("en2")
  int32_t type;       // the request number below
  int32_t value;      // a small value in or out (some gets answer here when no buffer is given)
  uint32_t length;    // the buffer's length (some answers set it to the length they have)
  uint32_t pad;
  uint64_t data;      // the user-space buffer
};
static_assert(sizeof(Req) == 40, "apple80211req is 40 bytes");
static_assert(__builtin_offsetof(Req, type) == 16 && __builtin_offsetof(Req, value) == 20 &&
              __builtin_offsetof(Req, length) == 24 && __builtin_offsetof(Req, data) == 32, "apple80211req's fields");

// ------------------------------------------------------------------------------------------ the request numbers (27.3)
// Decoded from 27.3's own name table (REPORT.md 2.2); 7, 8, 28, 29, 54 are from sheng's set (unchanged numbers, not re-read).
enum : int32_t {
  kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10,
  kScanResult = 11, kCardCapabilities = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19,
  kAssociate = 20, kAssociateResult = 21, kDisassociate = 22, kStatusDevName = 23, kSupportedChannels = 27, kLocale = 28,
  kDeauth = 29, kDriverVersion = 43, kHardwareVersion = 44, kRsnIe = 46, kCountryCode = 51, kRadioInfo = 54, kRsnConf = 77,
  kPmkCache = 83, kScanReqMultiple = 86, kVirtualIfRole = 96, kScanCacheClear = 90, kCurrentNetwork = 103, kOffloadRsn = 177, kChannelsInfo = 207,
  kHwSupportedChannels = 254, kScanControl = 360, kCurPmk = 376, kRsnXe = 457, kInfraEnumerated = 512, kHeCapability = 570,
  kSuspendInterface = 606, kAggressiveEdca = 607,
};

// The name of a request for the log (the 27.3 names of the ones above; "?" for the rest, which the log gives by number).
inline const char *requestName(int32_t type)
{
  switch (type) {
  case kSsid: return "SSID";
  case kAuthType: return "AUTH_TYPE";
  case kCipherKey: return "CIPHER_KEY";
  case kChannel: return "CHANNEL";
  case kPowerSave: return "POWERSAVE";
  case kTxPower: return "TXPOWER";
  case kRate: return "RATE";
  case kBssid: return "BSSID";
  case kScanReq: return "SCAN_REQ";
  case kScanResult: return "SCAN_RESULT";
  case kCardCapabilities: return "CARD_CAPABILITIES";
  case kState: return "STATE";
  case kPhyMode: return "PHY_MODE";
  case kOpMode: return "OP_MODE";
  case kRssi: return "RSSI";
  case kNoise: return "NOISE";
  case kPower: return "POWER";
  case kAssociate: return "ASSOCIATE";
  case kAssociateResult: return "ASSOCIATE_RESULT";
  case kDisassociate: return "DISASSOCIATE";
  case kStatusDevName: return "STATUS_DEV_NAME";
  case kSupportedChannels: return "SUPPORTED_CHANNELS";
  case kLocale: return "LOCALE";
  case kDeauth: return "DEAUTH";
  case kDriverVersion: return "DRIVER_VERSION";
  case kHardwareVersion: return "HARDWARE_VERSION";
  case kRsnIe: return "RSN_IE";
  case kCountryCode: return "COUNTRY_CODE";
  case kRadioInfo: return "RADIO_INFO";
  case kRsnConf: return "RSN_CONF";
  case kPmkCache: return "PMK_CACHE";
  case kScanReqMultiple: return "SCAN_REQ_MULTIPLE";
  case kScanCacheClear: return "SCANCACHE_CLEAR";
  case kVirtualIfRole: return "VIRTUAL_IF_ROLE";
  case kCurrentNetwork: return "CURRENT_NETWORK";
  case kOffloadRsn: return "OFFLOAD_RSN";
  case kChannelsInfo: return "CHANNELS_INFO";
  case kHwSupportedChannels: return "HW_SUPPORTED_CHANNELS";
  case kScanControl: return "SCAN_CONTROL";
  case kCurPmk: return "CUR_PMK";
  case kRsnXe: return "RSN_XE";
  case kInfraEnumerated: return "INFRA_ENUMERATED";
  case kHeCapability: return "HE_CAPABILITY";
  case kSuspendInterface: return "SUSPEND_INTERFACE";
  case kAggressiveEdca: return "AGGRESSIVE_EDCA";
  default: return "?";
  }
}

// --------------------------------------------------------------------------------- events (kernel events, class 6 / subclass 1)
// Vendor Apple, class KEV_IEEE80211_CLASS (6), subclass 1, the data: the interface's name (16 bytes) and then the payload.
// The codes are sheng's and AirportItlwm's historical ones (REPORT.md section 4: "prior-art evidence to test", not re-decoded).
constexpr uint32_t kEventClass = 6, kEventSubclass = 1;
enum : uint32_t { kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10 };

// The link-changed payload (sheng's 24A446 layout, 32 bytes; 27.3 not re-read).
struct LinkEvent {
  uint8_t down, unused0[3];
  int32_t rssi;
  uint16_t snr, noise;
  uint8_t unused1[4];
  uint8_t involuntary, unused2[3];
  uint32_t reason, subreason;
  uint8_t unused3[4];
};
static_assert(sizeof(LinkEvent) == 32, "link event layout");

// ------------------------------------------------------------------------------------------------------------- the values
enum : uint32_t { kStateInit = 0, kStateScan = 1, kStateAuth = 2, kStateAssoc = 3, kStateRun = 4 };
constexpr uint32_t kOpModeStation = 1;
// PHY modes (bit masks): auto, 11a, 11b, 11g, 11n, 11ac.
enum : uint32_t { kPhyAuto = 0x1, kPhy11a = 0x2, kPhy11b = 0x4, kPhy11g = 0x8, kPhy11n = 0x10, kPhy11ac = 0x80 };
constexpr uint32_t kPhySupported = kPhyAuto | kPhy11a | kPhy11b | kPhy11g | kPhy11n | kPhy11ac;
// Channel flags: 20 MHz, 2.4 GHz, 5 GHz, active (not passive-only). sheng's conservative set (REPORT.md 5: Ventura's menu wanted
// conservative scan-channel flags).
enum : uint32_t { kChan20MHz = 0x2, kChan2GHz = 0x8, kChan5GHz = 0x10, kChanActive = 0x80 };
// Card capabilities, the bit numbers of the bitmap: AES, AES-CCM, power management, short slot, short preamble, WPA2, short
// guard interval at 20 MHz, TSN — sheng's set (what this station does: WPA2-PSK with CCMP, power save, 11n). 27.3 (host analysis
// 2026-11-14, docs/drivers/wifi-menu.md section 5.2): a raw 23-byte bitmap from offset 0, no version word, asked by
// the bind (the only sanity request of the ioctl path: any error fails the bind); bit 46 would make airportd wait for a scan's
// completion event and bit 73 choose SCAN_REQ_MULTIPLE for directed scans: neither is set.
constexpr uint8_t kCapBits[] = {2, 3, 6, 9, 10, 14, 17, 19};
constexpr uint32_t kCapBytes = 23;
constexpr int kErrPowerOff = 82;      // EPWROFF on Darwin: "device power is off"
constexpr int kErrNotAssociated = 2;  // ENOENT (sheng's answer for what needs an association)
// EIO after the last scan result (sheng's; the cursor starts again). 27.3's synchronous scan ends its result loop well only on a
// get that returns exactly 5: whether the ioctl path hands it errno (then this works) or ioctl()'s -1 (then a scan over ioctls
// always fails and only a user client can end it) was not settled on the host: the M1 boot's first question
// (docs/drivers/wifi-menu.md section 5.7).
constexpr int kErrEndOfResults = 5;
constexpr int kErrUnsupported = 102;  // EOPNOTSUPP on Darwin
constexpr int kErrInval = 22, kErrFault = 14;

// ---------------------------------------------------------------------------------------------------------- the layouts
struct Channel {
  uint32_t version, channel, flags;
};
static_assert(sizeof(Channel) == 12, "channel");

// {version, count, channels}: 4824 bytes on 27.3 (count at 4, entries from 8, stride 12; user space does not bound the count:
// at most 401), as sheng's 24A446 list. Only the entries we have are written.
constexpr uint32_t kChannelsMax = 64;
struct ChannelList {
  uint32_t version, count;
  Channel list[kChannelsMax];
};
static_assert(sizeof(ChannelList) == 8 + 12 * kChannelsMax, "channel list");

// A scan result's head: the fields to the elements are sheng's 24A446 layout, which 27.3 keeps (the element length at 0x8a, preset
// by user space to its capacity, the elements at 0x8c). 27.3's record is 2280 bytes: 2048 bytes of elements, then fields from
// 0x88c (a request identifier user space puts there itself, a timestamp, an RSN priority, the AP's PHY mode, MLO flags). The
// answer writes the head and the elements it has, never the tail (resultBytes()); 1024 bytes of elements are more than the
// rebuilt ones ever take.
struct ScanResult {
  uint32_t version;              // 0x00
  Channel channel;               // 0x04
  uint16_t noiseKnown;           // 0x10
  int16_t noise, snr, rssi;      // 0x12 0x14 0x16
  uint16_t beaconInterval;       // 0x18
  uint16_t capability;           // 0x1a
  uint8_t bssid[6];              // 0x1c
  uint8_t rateCount, unused0;    // 0x22
  uint32_t rates[15];            // 0x24
  uint8_t ssidLength;            // 0x60
  uint8_t ssid[32];              // 0x61
  uint8_t unused1[3];
  uint32_t ageMs;                // 0x84
  uint8_t flags, unused2;        // 0x88
  uint16_t elementsLength;       // 0x8a
  uint8_t elements[1024];        // 0x8c
};
constexpr uint32_t kScanResultBytes27 = 2280;   // what airportd asks with (its SCAN_RESULT and CURRENT_NETWORK buffers)
static_assert(sizeof(ScanResult) == 1164 && __builtin_offsetof(ScanResult, rssi) == 0x16 &&
              __builtin_offsetof(ScanResult, bssid) == 0x1c && __builtin_offsetof(ScanResult, ssidLength) == 0x60 &&
              __builtin_offsetof(ScanResult, ageMs) == 0x84 && __builtin_offsetof(ScanResult, elementsLength) == 0x8a &&
              __builtin_offsetof(ScanResult, elements) == 0x8c, "scan result layout");

// POWER: {version, radios, state per radio}; 24 bytes on 27.3 as in sheng's. The set reads state[0]; airportd's set first gets the
// radio count, so radios must not be 0.
struct Power {
  uint32_t version, radios, state[4];
};
static_assert(sizeof(Power) == 24, "power");

// PHY_MODE: {version, supported modes, the active mode}.
struct PhyMode {
  uint32_t version, supported, active;
};
// RSSI and NOISE: {version, radios (0 is an error), unit (0 dBm), per chain[4], aggregate, extension per chain[4], its aggregate}:
// 52 bytes on 27.3 as in sheng's.
struct Rssi {
  uint32_t version, radios, unit;
  int32_t chain[4], aggregate, extChain[4], extAggregate;
};
static_assert(sizeof(Rssi) == 52, "rssi");

// DRIVER_VERSION / HARDWARE_VERSION: 27.3 asks with a 384-byte buffer and reads a NUL-terminated UTF-8 string from offset 0 (no
// version word, no number checked; sheng's layout). airportd's interface set-up fails without a DRIVER_VERSION answer.
constexpr uint32_t kVersionBytes = 384;
// STATUS_DEV_NAME: not asked by 27.3's bind; answered like the versions (a string from offset 0), its layout not re-read.
// COUNTRY_CODE: 3 bytes, a C string (27.3, as sheng's).

// SCAN_REQ (the parts read; 5528 bytes on 27.3, SCAN_REQ_MULTIPLE 5352): version, BSS type, BSSID, the SSID's length at 16 and
// its bytes at 20 (27.3, as sheng's).
constexpr uint32_t kScanReqSsidLengthAt = 16, kScanReqSsidAt = 20;

// ------------------------------------------------------------------------------------------------- our scan list, compact
// One network of PianoWifi's scan list (the registry property piano-wifi-scan, published by level 6: PianoWifi.cpp publishScan).
struct Bss {
  uint8_t bssid[6] = {};
  uint8_t ssid[kSsidMax] = {};
  uint8_t ssidLength = 0;
  bool hidden = false;
  uint16_t channel = 0;
  uint32_t freq = 0;
  int32_t rssi = 0;
  uint16_t capability = 0;       // the beacon's capability field (0 when the list predates it: then ESS, and privacy with an RSN)
  uint16_t beaconInterval = 0;   // TU (0: 100)
  uint8_t rates[16] = {};        // as the frame carried them (bit 7: basic), 500 kb/s units
  uint8_t rateCount = 0;
  uint8_t rsn[64] = {};          // the RSN element whole (48, length, body)
  uint8_t rsnLength = 0;
};

struct ScanSnapshot {
  Bss bss[kScanEntriesMax];
  uint32_t count = 0;
};

// "aa:bb:cc:dd:ee:ff" (the registry's form) into six bytes; false for anything else (a masked "**" included).
inline bool parseBssid(const char *text, uint8_t out[6])
{
  if (!text) return false;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (unsigned i = 0; i < 6; ++i) {
    const int hi = nibble(text[3 * i]), lo = hi < 0 ? -1 : nibble(text[3 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i < 5 ? text[3 * i + 2] != ':' : text[3 * i + 2] != 0) return false;
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

inline uint32_t channelOfFreq(uint32_t mhz)
{
  if (mhz == 2484) return 14;
  if (mhz >= 2412 && mhz < 2484) return (mhz - 2407) / 5;
  if (mhz >= 5000 && mhz < 5900) return (mhz - 5000) / 5;
  return 0;
}

inline void fillChannel(Channel &c, uint32_t mhz, uint32_t channel)
{
  c.version = 1;
  c.channel = channel ? channel : channelOfFreq(mhz);
  c.flags = kChan20MHz | (mhz >= 4900 ? kChan5GHz : kChan2GHz);
}

// The elements of a scan result, rebuilt from what the list keeps (the list does not keep the frame's elements): SSID (0), the
// supported rates (1, at most 8), the DS parameter set (3, 2.4 GHz), the extended rates (50, the rest) and the RSN element (48)
// as the frame carried it. Enough for the menu: the name, the band, the security. Returns the bytes written.
inline uint32_t buildElements(const Bss &b, uint8_t *out, uint32_t capacity)
{
  uint32_t n = 0;
  auto put = [&](uint8_t id, const uint8_t *body, uint32_t length) {
    if (n + 2 + length > capacity) return;
    out[n++] = id;
    out[n++] = uint8_t(length);
    for (uint32_t i = 0; i < length; ++i) out[n++] = body[i];
  };
  put(0, b.ssid, b.ssidLength <= kSsidMax ? b.ssidLength : kSsidMax);
  const uint32_t rates = b.rateCount <= sizeof b.rates ? b.rateCount : uint32_t(sizeof b.rates);
  if (rates) put(1, b.rates, rates < 8 ? rates : 8);
  if (b.freq && b.freq < 4900) {
    const uint8_t ch = uint8_t(b.channel ? b.channel : channelOfFreq(b.freq));
    put(3, &ch, 1);
  }
  if (rates > 8) put(50, b.rates + 8, rates - 8);
  if (b.rsnLength >= 2 && b.rsn[0] == 48 && uint32_t(b.rsn[1]) + 2 <= b.rsnLength && n + b.rsn[1] + 2u <= capacity) {
    for (uint32_t i = 0; i < uint32_t(b.rsn[1]) + 2; ++i) out[n++] = b.rsn[i];
  }
  return n;
}

// The bytes of a filled result to copy out: the head and the elements it has (never 27.3's tail, from 0x88c).
inline uint32_t resultBytes(const ScanResult &r) { return uint32_t(__builtin_offsetof(ScanResult, elements)) + r.elementsLength; }

inline void fillScanResult(ScanResult &r, const Bss &b)
{
  uint8_t *raw = reinterpret_cast<uint8_t *>(&r);
  for (uint32_t i = 0; i < sizeof r; ++i) raw[i] = 0;
  r.version = 1;
  fillChannel(r.channel, b.freq, b.channel);
  r.noiseKnown = 1;
  r.noise = -95;
  r.rssi = int16_t(b.rssi);
  r.snr = int16_t(b.rssi > -95 ? b.rssi + 95 : 0);
  r.beaconInterval = b.beaconInterval ? b.beaconInterval : 100;
  // ESS (bit 0); privacy (bit 4) whenever the network has an RSN element, should the list predate the capability field
  r.capability = uint16_t(b.capability ? b.capability : (0x1 | (b.rsnLength ? 0x10 : 0)));
  for (unsigned i = 0; i < 6; ++i) r.bssid[i] = b.bssid[i];
  const uint32_t rates = b.rateCount < 15 ? b.rateCount : 15;
  r.rateCount = uint8_t(rates);
  for (uint32_t i = 0; i < rates; ++i) r.rates[i] = uint32_t(b.rates[i] & 0x7f) / 2;   // Mb/s (the 5.5 rate reads 5)
  r.ssidLength = b.ssidLength <= kSsidMax ? b.ssidLength : kSsidMax;
  for (uint32_t i = 0; i < r.ssidLength; ++i) r.ssid[i] = b.ssid[i];
  r.ageMs = 0;
  r.elementsLength = uint16_t(buildElements(b, r.elements, sizeof r.elements));
}

// ----------------------------------------------------------------------------------------------------------- the handler
// The accept modes (machdep.piano_wifi_a80211, piano-wifi-apple80211-accept=N): what the requests airportd asks on the tablet,
// whose layouts we do not have, get. Each one switchable at run time, so one boot compares them (docs/drivers/wifi-menu.md 6).
enum : uint32_t {
  kAcceptInfraEnumerated = 0x1,   // INFRA_ENUMERATED (512) set: accepted (airportd's own value; its result was found ignored)
  kAcceptUnknownSets = 0x2,       // any other set without a handler: accepted (0), the value ignored
  kAcceptUnknownGets = 0x4,       // any other get without a handler: a zeroed buffer of the asked length (0 in req_val without one)
  kAcceptVirtualRole = 0x8,       // VIRTUAL_IF_ROLE (96) get: 0 in req_val (else refused: an answer may make the interface virtual)
};
constexpr uint32_t kAcceptDefault = kAcceptInfraEnumerated;

// The interface's apple80211 state (one per interface; the kext keeps it under its lock). No pointer in it: the kext keeps it in
// a data allocation.
struct State {
  bool power = true;              // POWER: the radio as airportd sees it (M1: off only stops the scans; the chip stays up)
  uint32_t cursor = 0;            // the next SCAN_RESULT
  bool cacheCleared = false;      // SCANCACHE_CLEAR since the last SCAN_REQ: no result until the next scan
  ScanSnapshot scan;              // the scan list as of the last refresh
  uint8_t mac[6] = {};
  char country[3] = {'X', 'X', 0};   // the world domain: the list keeps no Country element
  uint32_t scanRequests = 0, resultsServed = 0, listEnds = 0, powerSets = 0;
  uint32_t accept = kAcceptDefault;   // kAccept*: how requests without a layout of ours are answered (a runtime switch, M1b)
  uint32_t acceptedUnknown = 0;   // requests answered only because of `accept`
};

// A state as a fresh interface has it, in place (the kext's is a zeroed data allocation, too large for a temporary on the stack).
inline void reset(State &s)
{
  uint8_t *raw = reinterpret_cast<uint8_t *>(&s);
  for (uint32_t i = 0; i < sizeof s; ++i) raw[i] = 0;
  s.power = true;
  s.country[0] = 'X';
  s.country[1] = 'X';
  s.accept = kAcceptDefault;
}

// The identity strings (the kext's; the host test passes its own).
struct Identity {
  const char *driverVersion;   // "PianoWifi 1.0 (apple80211 M1)"
  const char *hardwareVersion; // "WCN7860"
  const char *devName;         // "PianoWifi"
};

// What the kext does after the answer, outside its lock.
enum : uint32_t {
  kDoPowerChanged = 0x1,   // post kEvPowerChanged
  kDoScanStarted = 0x2,    // arm the scan's completion (kEvScanDone after the time a short scan takes)
  kDoRefreshScan = 0x4,    // (asked before the handler runs for SCAN_REQ: the kext refreshes the snapshot first)
};

struct Answer {
  int error = 0;           // the ioctl's result (0 or an errno)
  uint32_t effects = 0;    // kDo*
  uint32_t wrote = 0;      // bytes copied out (the log line's "wrote")
  bool accepted = false;   // answered by an accept mode, not by a handler of ours
};

// The user-space copies (copyin/copyout in the kext, memcpy in the host test); each returns 0 or an errno.
struct UserIo {
  void *ctx;
  int (*copyIn)(void *ctx, uint64_t from, void *to, uint32_t length);
  int (*copyOut)(void *ctx, const void *from, uint64_t to, uint32_t length);
};

// The scratch buffer the kext gives the handler (never on the stack: the largest answer is the scan result, 1164 bytes, and the
// channel list; the caller's kernel stack is the requesting thread's 16 KiB).
constexpr uint32_t kScratchBytes = 2048;
static_assert(sizeof(ScanResult) <= kScratchBytes && sizeof(ChannelList) <= kScratchBytes && kVersionBytes <= kScratchBytes,
              "the scratch buffer holds every answer");

namespace detail {
inline void zero(uint8_t *p, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) p[i] = 0;
}
inline uint32_t clip(uint32_t a, uint32_t b) { return a < b ? a : b; }
// The answer copied into the caller's buffer, as much as fits (sheng's copy_out): a request without a buffer is refused.
inline int copyOut(Req &rq, const UserIo &io, const void *src, uint32_t size, Answer &a)
{
  if (rq.length == 0 || !rq.data) return kErrInval;
  const uint32_t n = clip(rq.length, size);
  const int e = io.copyOut(io.ctx, src, rq.data, n);
  if (!e) a.wrote = n;
  return e ? kErrFault : 0;
}
// A single value: in req_val when no buffer is given, else {version 1, value} (sheng's answer_value).
inline int value(Req &rq, const UserIo &io, uint32_t v, Answer &a)
{
  if (rq.length == 0 || !rq.data) {
    rq.value = int32_t(v);
    return 0;
  }
  const uint32_t w[2] = {1, v};
  return copyOut(rq, io, w, sizeof w, a);
}
// Raw bytes, their own length put in req_len (sheng's answer_bytes).
inline int bytes(Req &rq, const UserIo &io, const void *src, uint32_t size, Answer &a)
{
  if (!rq.data) return kErrInval;
  const uint32_t n = clip(rq.length, size);
  rq.length = size;
  const int e = n ? io.copyOut(io.ctx, src, rq.data, n) : 0;
  if (!e) a.wrote = n;
  return e ? kErrFault : 0;
}
// A zeroed answer of the asked length (kAcceptUnknownGets), in scratch-sized pieces; 0 in req_val when no buffer is given.
inline int zeros(Req &rq, const UserIo &io, uint8_t *scratch, uint32_t scratchBytes, Answer &a)
{
  if (rq.length == 0 || !rq.data) {
    rq.value = 0;
    return 0;
  }
  zero(scratch, scratchBytes);
  const uint32_t total = clip(rq.length, 64 * 1024);
  for (uint32_t at = 0; at < total;) {
    const uint32_t n = clip(total - at, scratchBytes);
    if (io.copyOut(io.ctx, scratch, rq.data + at, n)) return kErrFault;
    at += n;
  }
  a.wrote = total;
  return 0;
}
inline int copyIn(const Req &rq, const UserIo &io, void *dst, uint32_t size)
{
  zero(static_cast<uint8_t *>(dst), size);
  if (!rq.data || !rq.length) return kErrInval;
  return io.copyIn(io.ctx, rq.data, dst, clip(rq.length, size)) ? kErrFault : 0;
}
inline uint32_t strLen(const char *s, uint32_t max)
{
  uint32_t n = 0;
  while (s && n < max && s[n]) ++n;
  return n;
}
}  // namespace detail

// The channels this station scans (level 6's list: WifiLink.h, 2.4 GHz 1-13 and the 5 GHz non-radar ones).
inline uint32_t fillChannelList(ChannelList &out)
{
  static const uint16_t kFreqs[] = {2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472,
                                    5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825};
  detail::zero(reinterpret_cast<uint8_t *>(&out), sizeof out);
  out.version = 1;
  for (uint16_t f : kFreqs) {
    fillChannel(out.list[out.count], f, 0);
    out.list[out.count].flags |= kChanActive;
    ++out.count;
  }
  return 8 + 12 * out.count;
}

// One request. `scratch` holds kScratchBytes. The kext refreshes s.scan first for a SCAN_REQ / SCAN_REQ_MULTIPLE set.
inline Answer handle(State &s, const Identity &id, bool set, Req &rq, const UserIo &io, uint8_t *scratch)
{
  using namespace detail;
  Answer a;
  zero(scratch, kScratchBytes);
  uint32_t *w = reinterpret_cast<uint32_t *>(scratch);
  if (set) {
    switch (rq.type) {
    case kPower: {
      Power p;
      if ((a.error = copyIn(rq, io, &p, sizeof p))) return a;
      s.power = p.state[0] != 0;
      ++s.powerSets;
      a.effects |= kDoPowerChanged;
      return a;
    }
    case kScanReq:
    case kScanReqMultiple: {
      if (!s.power) {
        a.error = kErrPowerOff;
        return a;
      }
      // The SSID of a directed scan is read for the log only: the answer is the whole list (the radio's own scans hear them all).
      s.cursor = 0;
      s.cacheCleared = false;
      ++s.scanRequests;
      a.effects |= kDoScanStarted;
      return a;
    }
    case kScanCacheClear:
      s.scan.count = 0;
      s.cursor = 0;
      s.cacheCleared = true;
      return a;
    case kCountryCode:
    case kLocale:
    case kPowerSave:
      return a;   // accepted, nothing to change in M1
    case kAssociate:
    case kDisassociate:
    case kDeauth:
    case kCipherKey:
    case kAuthType:
    case kRsnIe:
      a.error = kErrUnsupported;   // the join's sets: M2, never accepted by a mode
      return a;
    case kInfraEnumerated:
      if (s.accept & kAcceptInfraEnumerated) {
        a.accepted = true;
        ++s.acceptedUnknown;
        return a;
      }
      a.error = kErrUnsupported;
      return a;
    default:
      if (s.accept & kAcceptUnknownSets) {
        a.accepted = true;
        ++s.acceptedUnknown;
        return a;
      }
      a.error = kErrUnsupported;
      return a;
    }
  }
  switch (rq.type) {
  case kDriverVersion:
  case kHardwareVersion:
  case kStatusDevName: {   // a NUL-terminated string from offset 0, the rest of the buffer zeros
    const char *text = rq.type == kDriverVersion ? id.driverVersion : rq.type == kHardwareVersion ? id.hardwareVersion : id.devName;
    const uint32_t n = strLen(text, kVersionBytes - 1);
    for (uint32_t i = 0; i < n; ++i) scratch[i] = uint8_t(text[i]);
    a.error = copyOut(rq, io, scratch, kVersionBytes, a);
    return a;
  }
  case kCardCapabilities: {
    for (uint8_t b : kCapBits) scratch[b / 8] |= uint8_t(1u << (b % 8));
    a.error = bytes(rq, io, scratch, kCapBytes, a);
    return a;
  }
  case kPower: {
    Power *p = reinterpret_cast<Power *>(scratch);
    p->version = 1;
    p->radios = 1;
    p->state[0] = s.power ? 1 : 0;
    a.error = copyOut(rq, io, p, sizeof *p, a);
    return a;
  }
  case kState:
    a.error = value(rq, io, kStateInit, a);   // never associated in M1 (on or off: INIT)
    return a;
  case kOpMode:
    a.error = value(rq, io, kOpModeStation, a);
    return a;
  case kPhyMode: {
    PhyMode *p = reinterpret_cast<PhyMode *>(scratch);
    p->version = 1;
    p->supported = kPhySupported;
    p->active = 0;   // no association
    a.error = copyOut(rq, io, p, sizeof *p, a);
    return a;
  }
  case kNoise: {
    Rssi *r = reinterpret_cast<Rssi *>(scratch);
    r->version = 1;
    r->radios = 1;
    for (int i = 0; i < 4; ++i) r->chain[i] = r->extChain[i] = -95;
    r->aggregate = r->extAggregate = -95;
    a.error = copyOut(rq, io, r, sizeof *r, a);
    return a;
  }
  case kRssi:
  case kChannel:
  case kSsid:
  case kBssid:
  case kCurrentNetwork:
  case kRate:
    a.error = kErrNotAssociated;   // what needs an association (M2)
    return a;
  case kAssociateResult:
    a.error = value(rq, io, 0, a);   // pending: nothing was asked
    return a;
  case kVirtualIfRole:   // an answer may make airportd take the interface for a virtual one: refused unless kAcceptVirtualRole
    if (s.accept & kAcceptVirtualRole) {
      a.accepted = true;
      ++s.acceptedUnknown;
      a.error = value(rq, io, 0, a);
      return a;
    }
    a.error = kErrUnsupported;
    return a;
  case kAuthType:
    w[0] = 1;
    w[1] = 1;   // open system
    a.error = copyOut(rq, io, w, 16, a);
    return a;
  case kCountryCode:
    a.error = bytes(rq, io, s.country, 3, a);
    return a;
  case kSupportedChannels:
  case kHwSupportedChannels: {
    ChannelList *l = reinterpret_cast<ChannelList *>(scratch);
    const uint32_t n = fillChannelList(*l);
    a.error = copyOut(rq, io, l, n, a);
    return a;
  }
  case kScanResult: {
    if (!s.power) {
      a.error = kErrPowerOff;
      return a;
    }
    if (s.cacheCleared || s.cursor >= s.scan.count) {
      s.cursor = 0;
      ++s.listEnds;
      a.error = kErrEndOfResults;
      return a;
    }
    ScanResult *r = reinterpret_cast<ScanResult *>(scratch);
    fillScanResult(*r, s.scan.bss[s.cursor++]);
    ++s.resultsServed;
    a.error = copyOut(rq, io, r, resultBytes(*r), a);
    return a;
  }
  default:
    if (s.accept & kAcceptUnknownGets) {
      a.accepted = true;
      ++s.acceptedUnknown;
      a.error = zeros(rq, io, scratch, kScratchBytes, a);
      return a;
    }
    a.error = kErrUnsupported;
    return a;
  }
}

// The SSID length a SCAN_REQ names (0: all networks), for the log; reads the request's buffer through `io`.
inline uint32_t scanReqSsidLength(const Req &rq, const UserIo &io)
{
  uint8_t head[kScanReqSsidAt];
  if (rq.length < kScanReqSsidAt || detail::copyIn(rq, io, head, sizeof head)) return 0;
  const uint32_t n = uint32_t(head[kScanReqSsidLengthAt]) | uint32_t(head[kScanReqSsidLengthAt + 1]) << 8 |
                     uint32_t(head[kScanReqSsidLengthAt + 2]) << 16 | uint32_t(head[kScanReqSsidLengthAt + 3]) << 24;
  return n <= kSsidMax ? n : 0;
}

// ------------------------------------------------------------------------------------------- the log: one line per kind
// A request's kind for the log: its number, direction, length and result. The kext logs each kind once (and counts the rest).
struct Seen {
  static constexpr uint32_t kMax = 192;
  uint64_t keys[kMax] = {};
  uint32_t count = 0, overflow = 0;
  void clear()
  {
    for (uint32_t i = 0; i < kMax; ++i) keys[i] = 0;
    count = overflow = 0;
  }
  // true the first time a kind is seen (and remembered); false after (and when the table is full: counted in overflow)
  bool first(int32_t type, bool set, uint32_t length, int error)
  {
    const uint64_t key = uint64_t(uint32_t(type)) << 32 | uint64_t(set ? 1u : 0u) << 31 | uint64_t(length & 0xffffu) << 8 |
                         uint64_t(uint32_t(error) & 0xffu);
    for (uint32_t i = 0; i < count; ++i)
      if (keys[i] == key) return false;
    if (count == kMax) {
      ++overflow;
      return false;
    }
    keys[count++] = key;
    return true;
  }
};

}  // namespace a80211
}  // namespace pianowifi
