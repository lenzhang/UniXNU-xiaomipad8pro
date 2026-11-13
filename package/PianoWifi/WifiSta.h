// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 7, the join: after level 6's scan, on the virtual device level 6 created, the network of the join profile is
// chosen and joined in the order this chip's own host uses (docs/drivers/wifi.md §13.3) — a portable step machine the owner
// steps with a clock, exactly as WifiLink.h's (host tests: tests/sta_host.cpp, against a firmware model). New code for this
// repository; the messages are WifiAssoc.h's, the frames WifiDot11.h's.
//
// The steps, each with its own deadline:
//   701 the join profile          <- machdep.piano_wifi_join (the owner hands it in: staSetProfile)
//   702 the network               -- the profile's network out of the scan list: the strongest of its access points, a 5 GHz
//                                    one first (the profile's access point when it names one)
//   703 a scan of our own         -> only when the list does not have it (at most piano-wifi-l7scans of them)
//   704 the station's parameters  -> 802.3 out and in, the stream count, the power-save parameters, power save off
//   705 the peer                  -> WMI_PEER_CREATE for the access point
//   706 its peer map              <- the data interface names the peer (HTT peer map)
//   707 its data set-up           -> the default routing, then one reorder queue per traffic identifier (0-16)
//   708 the device started        -> WMI_VDEV_START on the network's channel, its start response
//   709 authentication            -> Open System through WMI_MGMT_TX; its answer comes back as WMI_MGMT_RX
//   710 the association           -> the association request; its answer carries the status and the association identifier
//   711 the peer's association    -> WMI_PEER_ASSOC with its rates and (WPA2) the four-way-handshake flag; its confirmation
//   712 the station up            -> WMI_VDEV_UP with the network's address and the association identifier
//   713 authorised                -> an open network's peer is authorised at once; a WPA2 network's waits for the keys
//   714 associated                -- watched: a deauthentication, a disassociation, a kickout or a beacon miss ends it
//   715 the teardown              -> (a deauthentication of ours), the station down, the peer deleted, the device stopped
// Every command is written into the outbox (`out`), and the owner sends it and calls staSent: one command outstanding at a
// time, as on level 6. The log gets step names, status codes, counts and lengths: never the network's name, an address or a key.
// Nothing here allocates, maps or sleeps.
#pragma once
#include <stdint.h>
#include "WifiAssoc.h"
#include "WifiDot11.h"
#include "WifiHtt.h"
#include "WifiWmi.h"

namespace pianowifi {

enum StaStep : int {
  kStaIdle = 700, kStaProfileWait = 701, kStaChoose = 702, kStaRescan = 703, kStaParams = 704, kStaPeerCreate = 705,
  kStaPeerMap = 706, kStaPeerData = 707, kStaVdevStart = 708, kStaAuth = 709, kStaAssoc = 710, kStaPeerAssoc = 711,
  kStaVdevUp = 712, kStaAuthorize = 713, kStaAssociated = 714, kStaTeardown = 715, kStaDone = 716, kStaStopped = 717,
};

enum StaError : int {
  kStaErrNone = 0, kStaErrNoProfile = -1, kStaErrNotFound = -2, kStaErrSecurity = -3, kStaErrTimeout = -4,
  kStaErrStartRefused = -5, kStaErrAuthRefused = -6, kStaErrAssocRefused = -7, kStaErrNoAnswer = -8, kStaErrBuild = -9,
  kStaErrDisconnected = -10, kStaErrStopped = -11, kStaErrMemory = -12,
};

// piano-wifi-l7 (a bitmask; a decimal value is always safe to write)
constexpr uint32_t kStaFlagParams = 1u << 0;      // the station's parameters after the virtual device (704)
constexpr uint32_t kStaFlagPeerData = 1u << 1;    // the peer's default routing and reorder queues (707)
constexpr uint32_t kStaFlagPeerMap = 1u << 2;     // wait for the data interface's peer map (706)
constexpr uint32_t kStaFlagPrefer5 = 1u << 3;     // a 5 GHz access point before a 2.4 GHz one
constexpr uint32_t kStaFlagRadarOk = 1u << 4;     // a radar channel may be chosen (joined passively: the device listens first)
constexpr uint32_t kStaFlagDeauth = 1u << 5;      // the teardown says goodbye with a deauthentication of ours
constexpr uint32_t kStaFlagNativeWifi = 1u << 6;  // native Wi-Fi frames out and in instead of 802.3 (the A/B of 704's format)
constexpr uint32_t kStaFlagNoJoin = 1u << 7;      // choose and log the network, then stop: nothing is created (a dry run)
constexpr uint32_t kStaFlagsDefault = kStaFlagParams | kStaFlagPeerData | kStaFlagPeerMap | kStaFlagPrefer5 |
                                      kStaFlagRadarOk | kStaFlagDeauth;

// What the owner reports as it happens (the kext prints a line for each; the tests count them).
enum StaReport : uint32_t {
  kStaRepChosen = 1,       // a: the list index, b: how many access points carry the name
  kStaRepPeerMap,          // a: the peer identifier, b: the hardware peer
  kStaRepStartResp,        // a: the status, b: milliseconds after the command
  kStaRepAuthAnswer,       // a: the status, b: milliseconds after the frame
  kStaRepAssocAnswer,      // a: the status, b: the association identifier
  kStaRepTxStatus,         // a: the descriptor, b: the status
  kStaRepPeerAssocConf,    // a: 1 when confirmed, b: milliseconds
  kStaRepDisconnect,       // a: the kind (StaDisconnect), b: the reason
  kStaRepTry,              // a: the step, b: the try (1, 2, ...)
  kStaRepScanDone,         // a: the rescan's number, b: how many entries the list has now
  kStaRepTeardown,         // a: the piece (StaTeardownPiece), b: 1 answered / 0 not
};

enum StaDisconnect : uint32_t { kStaDiscNone = 0, kStaDiscDeauth, kStaDiscDisassoc, kStaDiscKickout, kStaDiscBeaconMiss };
enum StaTeardownPiece : uint32_t { kStaTdDeauth = 1, kStaTdDown, kStaTdPeerDelete, kStaTdStop };

inline const char *staStepName(int step)
{
  switch (step) {
  case kStaIdle: return "idle";
  case kStaProfileWait: return "waiting for the join profile";
  case kStaChoose: return "the network";
  case kStaRescan: return "a scan of our own";
  case kStaParams: return "the station's parameters";
  case kStaPeerCreate: return "the peer";
  case kStaPeerMap: return "the peer's map";
  case kStaPeerData: return "the peer's data set-up";
  case kStaVdevStart: return "the device started";
  case kStaAuth: return "authentication";
  case kStaAssoc: return "the association";
  case kStaPeerAssoc: return "the peer's association";
  case kStaVdevUp: return "the station up";
  case kStaAuthorize: return "authorised";
  case kStaAssociated: return "associated";
  case kStaTeardown: return "the teardown";
  case kStaDone: return "done";
  default: return "stopped";
  }
}

inline const char *staErrorName(int error)
{
  switch (error) {
  case kStaErrNone: return "none";
  case kStaErrNoProfile: return "no join profile was set";
  case kStaErrNotFound: return "the profile's network is not in the scan list";
  case kStaErrSecurity: return "the network's security is not the profile's or not supported";
  case kStaErrTimeout: return "a step timed out";
  case kStaErrStartRefused: return "the firmware refused to start the device";
  case kStaErrAuthRefused: return "the access point refused the authentication";
  case kStaErrAssocRefused: return "the access point refused the association";
  case kStaErrNoAnswer: return "the access point did not answer";
  case kStaErrBuild: return "a message could not be built";
  case kStaErrDisconnected: return "the access point ended the association";
  case kStaErrStopped: return "the owner stopped the join";
  default: return "no memory for the frames or the queues";
  }
}

inline const char *staDisconnectName(uint32_t kind)
{
  switch (kind) {
  case kStaDiscDeauth: return "a deauthentication";
  case kStaDiscDisassoc: return "a disassociation";
  case kStaDiscKickout: return "the firmware's kickout";
  case kStaDiscBeaconMiss: return "a beacon miss";
  default: return "nothing";
  }
}

struct StaOptions {
  uint32_t flags = kStaFlagsDefault;
  uint32_t vdevId = 0;
  uint32_t chains = 2;                 // the radio's chains: the device's stream count (704)
  uint32_t profileWaitMs = kAssocProfileWaitMs;
  uint32_t stepMs = 3000;              // a command's answer (the start response, the peer map, the confirmation)
  uint32_t frameWaitMs = kAssocFrameWaitMs, frameTries = kAssocFrameTries;
  uint32_t holdMs = kAssocHoldMs;      // associated: how long the watch runs; 0: until the owner stops it
  uint32_t rescans = 2, scanWaitMs = kScanWaitMsDefault;
  uint32_t paceMs = 20;                // between the many small commands of 704 and 707
  int32_t minRssi5 = -82;              // a 5 GHz access point weaker than this loses to a stronger 2.4 GHz one
};

struct StaDma {
  uint8_t *va = nullptr;
  uint64_t bus = 0;
  uint32_t bytes = 0;
};

// What the owner gives the machine: the device's own address, level 6's results, the data interface's state, and window memory
// for the frames (the firmware reads a frame by DMA until its completion) and the 17 reorder queues.
struct StaEnv {
  const uint8_t *ownMac = nullptr;
  const ScanList *scan = nullptr;
  const RegDomain *reg = nullptr;
  const ChanList *chanList = nullptr;
  const HttRx *htt = nullptr;           // null: the data interface is not connected (no peer map to wait for)
  StaDma frame[kAssocFrameBuffers];
  StaDma reoq;                          // at least 16 x 512 + 128 bytes, 512-byte aligned
};

struct StaChoice {
  bool found = false;
  uint32_t index = 0, candidates = 0, excludedRadar = 0;
  uint32_t freq = 0, channel = 0;
  int32_t rssi = 0;
  bool band5 = false, passive = false, radar = false;
  int32_t powerDbm = 20;
  ScanEntry net;                        // a copy: a rescan may change the list
};

constexpr uint32_t kStaOutBytes = 768;
constexpr unsigned kStaFrameAuth = 0, kStaFrameAssoc = 1, kStaFrameDeauth = 2;

struct Sta {
  StaOptions options;
  StaEnv env;
  int step = kStaIdle, error = kStaErrNone, failedStep = 0;
  uint64_t began = 0, deadline = 0, pace = 0, sentAt = 0, stepAt = 0;
  // the profile (the name's bytes are kept only to compare them with the scan list's)
  bool profileSet = false, bssidGiven = false;
  uint8_t ssid[kSsidMax] = {}, ssidLength = 0, security = 0, bssidPref[6] = {};
  StaChoice choice;
  uint8_t bssid[6] = {};
  bool rsn = false;
  uint8_t groupCipher = 0, pairwise = 0, akm = 0;
  // the outbox: one command waiting to be sent, and what it is
  uint8_t out[kStaOutBytes] = {};
  uint32_t outLength = 0;
  int outStep = 0;
  uint32_t commands = 0, lastCommandStep = 0;
  // the steps' progress
  uint32_t paramsSent = 0, tidsSent = 0, rescansDone = 0, scanId = 0;
  bool routingSent = false, scanAsked = false, scanDone = false;
  bool peerCreated = false, peerMapped = false, vdevStartSent = false, vdevStarted = false, vdevUp = false, authorized = false;
  uint32_t peerId = 0, hwPeerId = 0;
  VdevStartResp startResp;
  uint32_t tries = 0, descId = 0;
  uint16_t seq = 0;
  bool frameOut = false;                // the current frame went out (its answer is waited for)
  bool authAnswered = false, assocAnswered = false, peerAssocConf = false;
  uint16_t authStatus = 0, assocStatus = 0, aid = 0, assocCapability = 0;
  Dot11Answer assocAnswer;
  uint32_t authMs = 0, assocMs = 0;
  uint32_t txCompletions = 0, txAcked = 0, txLastStatus = 0;
  // the watch
  uint64_t associatedAt = 0;
  uint32_t disconnect = kStaDiscNone, disconnectReason = 0, disconnectMs = 0;
  // the teardown
  bool stopAsked = false, deauthSent = false, downSent = false, peerDeleteSent = false, peerDeleted = false, stopSent = false,
       vdevStopped = false;
  uint32_t tdPhase = 0;
  uint64_t teardownWait = 0;
  // counters
  uint32_t events = 0, mgmtSeen = 0, mgmtOurs = 0, mgmtOthers = 0;
  void (*note)(void *ctx, int step, const char *what) = nullptr;
  void (*report)(void *ctx, uint32_t what, uint32_t a, uint32_t b) = nullptr;
  void *ctx = nullptr;
};

namespace stadetail {

inline bool sameMac(const uint8_t *a, const uint8_t *b)
{
  for (unsigned i = 0; i < 6; ++i)
    if (a[i] != b[i]) return false;
  return true;
}

inline void enter(Sta &s, int step, uint64_t nowMs, uint32_t waitMs)
{
  s.step = step;
  s.stepAt = nowMs;
  s.deadline = nowMs + waitMs;
  if (s.note) s.note(s.ctx, step, staStepName(step));
}

inline void say(Sta &s, uint32_t what, uint32_t a, uint32_t b)
{
  if (s.report) s.report(s.ctx, what, a, b);
}

// The teardown follows any failure after something was created; a failure before that only stops the machine.
inline void beginTeardown(Sta &s, uint64_t nowMs)
{
  s.outLength = 0;
  enter(s, kStaTeardown, nowMs, s.options.stepMs * 4);
  s.teardownWait = 0;
  s.tdPhase = 0;
}

inline int fail(Sta &s, int error, uint64_t nowMs)
{
  s.failedStep = s.step;
  s.error = error;
  if (s.peerCreated || s.vdevStartSent) {
    beginTeardown(s, nowMs);
  } else {
    s.outLength = 0;
    s.step = kStaStopped;
    if (s.note) s.note(s.ctx, kStaStopped, staStepName(kStaStopped));
  }
  return error;
}

// The outbox: a WMI message built in place.
template <class Build>
inline bool post(Sta &s, Build build)
{
  WmiWriter w;
  const uint32_t length = build(w, s.out, kStaOutBytes);
  if (!length) return false;
  s.outLength = length;
  s.outStep = s.step;
  return true;
}

// The band, the channel and its regulatory facts of a frequency.
inline void channelFacts(const Sta &s, StaChoice &c)
{
  c.band5 = c.freq >= 4900;
  c.channel = c.band5 ? (c.freq - 5000) / 5 : (c.freq == 2484 ? 14u : (c.freq - 2407) / 5);
  c.passive = false;
  c.radar = false;
  c.powerDbm = 20;
  if (s.env.reg && s.env.reg->seen) {
    RegChannel rc;
    if (regChannel(*s.env.reg, c.freq, rc) && rc.allowed) {
      c.passive = rc.passive;
      c.radar = rc.radar;
      c.powerDbm = rc.powerDbm > 0 ? rc.powerDbm : 20;
      return;
    }
  }
  // no regulatory word on it: the 5 GHz radar range is treated as a radar channel, as the fallback channel list does
  c.radar = c.band5 && c.freq >= 5260 && c.freq <= 5700;
  c.passive = c.radar;
}

// The profile's network out of the scan list: the names compared byte for byte; the profile's access point when it names one
// that is there; else the strongest, a 5 GHz one first unless it is weaker than minRssi5 and a 2.4 GHz one is there.
inline StaChoice choose(const Sta &s)
{
  StaChoice best;
  if (!s.env.scan) return best;
  const ScanList &list = *s.env.scan;
  int bestIndex = -1;
  int32_t bestScore = -100000;
  for (uint32_t i = 0; i < list.count; ++i) {
    const ScanEntry &e = list.entries[i];
    if (e.hidden || e.ssidLength != s.ssidLength) continue;
    bool same = true;
    for (uint32_t k = 0; k < s.ssidLength && same; ++k) same = e.ssidRaw[k] == s.ssid[k];
    if (!same || !e.freq) continue;
    ++best.candidates;
    StaChoice c;
    c.freq = e.freq;
    channelFacts(s, c);
    if (c.radar && !(s.options.flags & kStaFlagRadarOk)) {
      ++best.excludedRadar;
      continue;
    }
    int32_t score = e.rssi;
    if (s.bssidGiven && sameMac(e.bssid, s.bssidPref)) score += 10000;   // the profile's access point wins outright
    if ((s.options.flags & kStaFlagPrefer5) && c.band5 && e.rssi >= s.options.minRssi5) score += 1000;
    if (score > bestScore) {
      bestScore = score;
      bestIndex = int(i);
    }
  }
  if (bestIndex < 0) return best;
  const ScanEntry &e = list.entries[bestIndex];
  best.found = true;
  best.index = uint32_t(bestIndex);
  best.freq = e.freq;
  best.rssi = e.rssi;
  best.net = e;
  channelFacts(s, best);
  return best;
}

// The security the profile wants against the network's: an open profile needs an open network; WPA2-PSK needs an RSN element
// with PSK key management and CCMP, and no required management frame protection (not in the first version). Returns false
// when the join must not be tried.
inline bool securityFits(Sta &s)
{
  const ScanEntry &n = s.choice.net;
  const ScanSecurity &sec = n.security;
  if (s.security == 0) {   // open
    s.rsn = false;
    return !(n.capability & kDot11CapPrivacy) && !sec.rsn && !sec.wpa1;
  }
  if (!sec.rsn || sec.truncated) return false;
  if (!(sec.akm & (1u << kRsnAkmPsk))) return false;
  if (!(sec.pairwise & (1u << kRsnCipherCcmp))) return false;
  if (sec.rsnCaps & kRsnCapMfpRequired) return false;
  if (sec.groupCipher != kRsnCipherCcmp && sec.groupCipher != kRsnCipherTkip) return false;
  s.rsn = true;
  s.groupCipher = sec.groupCipher;
  s.pairwise = kRsnCipherCcmp;
  s.akm = kRsnAkmPsk;
  return true;
}

// The station's parameters, in the order this chip's own host sends them after a virtual device: the transmit
// encapsulation, the receive decapsulation, the stream count, the three power-save parameters, power save off.
constexpr uint32_t kStaParamCount = 7;

inline bool postParam(Sta &s, uint32_t index)
{
  const uint32_t vdev = s.options.vdevId;
  const uint32_t format = (s.options.flags & kStaFlagNativeWifi) ? kWmiTxRxNativeWifi : kWmiTxRxEthernet;
  const uint32_t nss = s.options.chains ? s.options.chains : 1;
  return post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) -> uint32_t {
    switch (index) {
    case 0: return assocBuildVdevSetParam(w, b, c, vdev, kWmiVdevParamTxEncapType, format);
    case 1: return assocBuildVdevSetParam(w, b, c, vdev, kWmiVdevParamRxDecapType, format);
    case 2: return assocBuildVdevSetParam(w, b, c, vdev, kWmiVdevParamNss, nss);
    case 3: return assocBuildStaPsParam(w, b, c, vdev, kWmiStaPsParamRxWakePolicy, kWmiStaPsRxWakePolicyWake);
    case 4: return assocBuildStaPsParam(w, b, c, vdev, kWmiStaPsParamTxWakeThreshold, kWmiStaPsTxWakeThresholdAlways);
    case 5: return assocBuildStaPsParam(w, b, c, vdev, kWmiStaPsParamPspollCount, kWmiStaPsPspollCountNoMax);
    default: return assocBuildPowersaveOff(w, b, c, vdev);
    }
  });
}

// A queue descriptor's place in the owner's memory: the 16 QoS ones of 512 bytes, then the non-QoS one.
inline uint32_t reoqOffset(uint32_t tid) { return tid == kStaNonQosTid ? 16 * kReoQueueQosBytes : tid * kReoQueueQosBytes; }
constexpr uint32_t kStaReoqBytes = 16 * kReoQueueQosBytes + kReoQueueNonQosBytes;

inline bool postFrame(Sta &s, unsigned which, uint32_t length)
{
  const StaDma &d = s.env.frame[which];
  MgmtTx tx;
  tx.vdevId = s.options.vdevId;
  tx.descId = ++s.descId;
  tx.freq = 0;   // the device's own channel
  tx.bus = d.bus;
  tx.frame = d.va;
  tx.bytes = length;
  return post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildMgmtTx(w, b, c, tx); });
}

inline bool postAuth(Sta &s)
{
  const StaDma &d = s.env.frame[kStaFrameAuth];
  const uint32_t length = dot11BuildAuth(d.va, d.bytes, s.env.ownMac, s.bssid, ++s.seq);
  return length && postFrame(s, kStaFrameAuth, length);
}

inline bool postAssoc(Sta &s)
{
  const StaDma &d = s.env.frame[kStaFrameAssoc];
  AssocRequest r;
  r.own = s.env.ownMac;
  r.bssid = s.bssid;
  r.ssid = s.ssid;
  r.ssidLength = s.ssidLength;
  r.band5 = s.choice.band5;
  r.seq = ++s.seq;
  r.net = &s.choice.net;
  r.rsn = s.rsn;
  r.groupCipher = s.groupCipher;
  r.pairwise = s.pairwise;
  r.akm = s.akm;
  r.maxPowerDbm = int8_t(s.choice.powerDbm);
  // the supported channels of the spectrum-management pair: the 5 GHz sub-band the network is in, four channels of 20 MHz
  r.firstChannel = uint8_t(s.choice.channel >= 149 ? 149 : s.choice.channel >= 100 ? 100 : s.choice.channel >= 52 ? 52 : 36);
  r.channelCount = uint8_t(s.choice.channel >= 149 ? 5 : s.choice.channel >= 100 ? 12 : 4);
  const uint32_t length = dot11BuildAssocRequest(d.va, d.bytes, r);
  return length && postFrame(s, kStaFrameAssoc, length);
}

inline uint32_t phyMode(const Sta &s)
{
  if (s.choice.band5) return kWmiModeLegacy5;
  for (uint32_t i = 0; i < s.choice.net.rateCount; ++i)
    if (dot11RateIsOfdm(s.choice.net.rates[i])) return kWmiModeLegacy24;
  return kWmiModeLegacy24Cck;
}

inline bool postPeerAssoc(Sta &s)
{
  PeerAssoc a;
  a.vdevId = s.options.vdevId;
  a.aid = s.aid;
  for (unsigned i = 0; i < 6; ++i) a.peer[i] = s.bssid[i];
  a.capability = s.assocCapability;
  a.listenInterval = kStaListenInterval;
  // the rates the association answer confirms, else the ones we offered
  uint8_t offered[16] = {};
  uint32_t count = 0;
  if (s.assocAnswer.rateCount) {
    for (uint32_t i = 0; i < s.assocAnswer.rateCount && count < sizeof offered; ++i)
      if (dot11RateUsable(s.assocAnswer.rates[i], s.choice.band5)) offered[count++] = s.assocAnswer.rates[i];
  }
  if (!count) count = dot11OfferRates(s.choice.net, s.choice.band5, offered, sizeof offered);
  for (uint32_t i = 0; i < count && i < sizeof a.legacyRates; ++i) a.legacyRates[i] = dot11RateForFirmware(offered[i]);
  a.legacyRateCount = count;
  a.phyMode = phyMode(s);
  a.qos = false;
  a.needPtk4Way = s.rsn;
  a.needGtk2Way = false;
  a.pmf = false;
  a.newAssoc = true;
  return post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildPeerAssoc(w, b, c, a); });
}

inline bool postVdevStart(Sta &s)
{
  VdevStart v;
  v.vdevId = s.options.vdevId;
  v.freq = s.choice.freq;
  v.mode = phyMode(s);
  v.beaconIntervalTu = s.choice.net.beaconInterval ? s.choice.net.beaconInterval : 100;
  v.dtimPeriod = s.choice.net.dtimPeriod ? s.choice.net.dtimPeriod : 1;
  v.txStreams = s.options.chains ? s.options.chains : 1;
  v.rxStreams = v.txStreams;
  v.maxPowerDbm = s.choice.powerDbm;
  v.passive = s.choice.passive;
  v.radar = false;   // the radar flag is an access point's business; a station's channel is only passive
  v.pmf = false;
  return post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildVdevStart(w, b, c, v); });
}

}  // namespace stadetail

// The machine begins: the owner's environment and options, and a deadline for the profile.
inline void staBegin(Sta &s, const StaOptions &options, const StaEnv &env, uint64_t nowMs)
{
  const auto note = s.note;
  const auto report = s.report;
  void *const ctx = s.ctx;
  // A profile handed in before the machine began (the kext hands it whenever it is set) is kept.
  const bool hadProfile = s.profileSet;
  uint8_t ssid[kSsidMax] = {}, bssid[6] = {};
  const uint8_t ssidLength = s.ssidLength, security = s.security;
  const bool bssidGiven = s.bssidGiven;
  for (unsigned i = 0; i < kSsidMax; ++i) ssid[i] = s.ssid[i];
  for (unsigned i = 0; i < 6; ++i) bssid[i] = s.bssidPref[i];
  s = Sta{};
  s.note = note;
  s.report = report;
  s.ctx = ctx;
  if (hadProfile) {
    s.profileSet = true;
    s.ssidLength = ssidLength;
    s.security = security;
    s.bssidGiven = bssidGiven;
    for (unsigned i = 0; i < kSsidMax; ++i) s.ssid[i] = ssid[i];
    for (unsigned i = 0; i < 6; ++i) s.bssidPref[i] = bssid[i];
  }
  for (unsigned i = 0; i < kSsidMax; ++i) ssid[i] = 0;
  s.options = options;
  s.env = env;
  s.began = nowMs;
  stadetail::enter(s, kStaProfileWait, nowMs, options.profileWaitMs);
}

// The profile: the network's name (1-32 bytes), its security (0 open, 1 WPA2-PSK) and, when given, the access point to
// prefer. The passphrase is not the kernel's business at this level (the four-way handshake is user space's, piece 3).
inline bool staSetProfile(Sta &s, const uint8_t *ssid, uint32_t ssidLength, uint8_t security, const uint8_t *bssid)
{
  if (!ssid || !ssidLength || ssidLength > kSsidMax || security > 1) return false;
  s.ssidLength = uint8_t(ssidLength);
  for (uint32_t i = 0; i < kSsidMax; ++i) s.ssid[i] = i < ssidLength ? ssid[i] : 0;
  s.security = security;
  s.bssidGiven = bssid != nullptr;
  for (unsigned i = 0; i < 6; ++i) s.bssidPref[i] = bssid ? bssid[i] : 0;
  s.profileSet = true;
  return true;
}

// The owner sent the outbox's command.
inline void staSent(Sta &s, uint64_t nowMs)
{
  s.outLength = 0;
  s.sentAt = nowMs;
  s.pace = nowMs + s.options.paceMs;
  s.lastCommandStep = uint32_t(s.outStep);
  ++s.commands;
}

// The owner wants the join to end (a suspend, the level's deadline): the teardown runs from wherever the machine is. A
// suspend has little time: `answerWaitMs` then bounds each answer the teardown waits for.
inline void staStop(Sta &s, uint64_t nowMs, uint32_t answerWaitMs = 0)
{
  if (answerWaitMs) s.options.stepMs = answerWaitMs;
  if (s.step == kStaDone || s.step == kStaStopped || s.step == kStaTeardown) return;
  s.stopAsked = true;
  if (!s.error) s.error = kStaErrStopped;
  s.failedStep = s.step;
  stadetail::fail(s, s.error, nowMs);
}

inline bool staFinished(const Sta &s) { return s.step == kStaDone || s.step == kStaStopped; }

// An event of the firmware's message interface: true when it was the join's (the owner then gives it to nobody else).
// Management frames that are not the join's (beacons, other networks' frames) are left to the owner's scan list.
inline bool staOnEvent(Sta &s, uint32_t id, const uint8_t *payload, uint32_t length, uint64_t nowMs)
{
  using namespace stadetail;
  if (s.step == kStaIdle || s.step == kStaDone || s.step == kStaStopped) return false;
  ++s.events;
  switch (id) {
  case kWmiEvtVdevStartResp: {
    VdevStartResp r;
    if (!assocParseVdevStartResp(r, payload, length) || r.vdevId != s.options.vdevId) return false;
    s.startResp = r;
    if (s.step == kStaVdevStart) {
      say(s, kStaRepStartResp, r.status, uint32_t(nowMs - s.sentAt));
      if (r.status != kWmiVdevStartOk) {
        fail(s, kStaErrStartRefused, nowMs);
        return true;
      }
      s.vdevStarted = true;
      s.tries = 0;
      s.frameOut = false;
      enter(s, kStaAuth, nowMs, s.options.frameWaitMs);
    }
    return true;
  }
  case kWmiEvtMgmtTxCompletion: {
    MgmtTxComplete c;
    if (!assocParseMgmtTxComplete(c, payload, length)) return false;
    if (!c.descId || c.descId > s.descId) return false;   // not one of ours
    ++s.txCompletions;
    if (c.status == kWmiMgmtTxStatusOk) ++s.txAcked;
    s.txLastStatus = c.status;
    say(s, kStaRepTxStatus, c.descId, c.status);
    return true;
  }
  case kWmiEvtMgmtRx: {
    WmiMgmtRx rx;
    if (!wmiParseMgmtRx(rx, payload, length) || (rx.status & kWmiRxStatusBad)) return false;
    ++s.mgmtSeen;
    if (!s.choice.found) return false;
    const Dot11Answer a = dot11Classify(rx.frame, rx.bytes, s.env.ownMac, s.bssid);
    if (!a.fromBssid || a.kind == kDot11AnsNone || a.kind == kDot11AnsOther) {
      ++s.mgmtOthers;
      return false;   // a beacon or someone else's: the scan list's
    }
    if (a.kind == kDot11AnsAuth && a.toUs && a.authSeq == kDot11AuthRespSeq) {
      ++s.mgmtOurs;
      if (s.step != kStaAuth || s.authAnswered) return true;
      s.authAnswered = true;
      s.authStatus = a.status;
      s.authMs = uint32_t(nowMs - s.sentAt);
      say(s, kStaRepAuthAnswer, a.status, s.authMs);
      if (a.status != kDot11StatusSuccess) {
        fail(s, kStaErrAuthRefused, nowMs);
        return true;
      }
      s.tries = 0;
      s.frameOut = false;
      enter(s, kStaAssoc, nowMs, s.options.frameWaitMs * 2);
      return true;
    }
    if (a.kind == kDot11AnsAssoc && a.toUs) {
      ++s.mgmtOurs;
      if (s.step != kStaAssoc || s.assocAnswered) return true;
      s.assocAnswered = true;
      s.assocStatus = a.status;
      s.assocMs = uint32_t(nowMs - s.sentAt);
      s.assocAnswer = a;
      say(s, kStaRepAssocAnswer, a.status, a.aid);
      if (a.status != kDot11StatusSuccess) {
        fail(s, kStaErrAssocRefused, nowMs);
        return true;
      }
      s.aid = a.aid;
      s.assocCapability = a.capability;
      enter(s, kStaPeerAssoc, nowMs, s.options.stepMs);
      if (!postPeerAssoc(s)) fail(s, kStaErrBuild, nowMs);
      return true;
    }
    if ((a.kind == kDot11AnsDeauth || a.kind == kDot11AnsDisassoc) && (a.toUs || a.broadcast)) {
      ++s.mgmtOurs;
      if (s.disconnect) return true;
      s.disconnect = a.kind == kDot11AnsDeauth ? kStaDiscDeauth : kStaDiscDisassoc;
      s.disconnectReason = a.reason;
      s.disconnectMs = s.associatedAt ? uint32_t(nowMs - s.associatedAt) : 0;
      say(s, kStaRepDisconnect, s.disconnect, a.reason);
      if (s.step == kStaAssociated) beginTeardown(s, nowMs);
      else if (s.step != kStaTeardown) fail(s, kStaErrDisconnected, nowMs);
      return true;
    }
    ++s.mgmtOthers;
    return false;
  }
  case kWmiEvtPeerAssocConf: {
    PeerAssocConf c;
    if (!assocParsePeerAssocConf(c, payload, length) || c.vdevId != s.options.vdevId) return false;
    if (s.step == kStaPeerAssoc && !s.peerAssocConf) {
      s.peerAssocConf = true;
      say(s, kStaRepPeerAssocConf, 1, uint32_t(nowMs - s.sentAt));
    }
    return true;
  }
  case kWmiEvtPeerStaKickout: {
    PeerKickout k;
    if (!assocParseKickout(k, payload, length) || !s.choice.found || !sameMac(k.peer, s.bssid)) return false;
    if (!s.disconnect && (s.step == kStaAssociated || s.step == kStaAuthorize || s.step == kStaVdevUp)) {
      s.disconnect = kStaDiscKickout;
      s.disconnectReason = k.reason;
      s.disconnectMs = s.associatedAt ? uint32_t(nowMs - s.associatedAt) : 0;
      say(s, kStaRepDisconnect, s.disconnect, k.reason);
      beginTeardown(s, nowMs);
    }
    return true;
  }
  case kWmiEvtRoam: {
    RoamEvent r;
    if (!assocParseRoam(r, payload, length) || r.vdevId != s.options.vdevId) return false;
    if (r.reason == kWmiRoamReasonBeaconMiss && !s.disconnect && s.step == kStaAssociated) {
      s.disconnect = kStaDiscBeaconMiss;
      s.disconnectReason = r.reason;
      s.disconnectMs = uint32_t(nowMs - s.associatedAt);
      say(s, kStaRepDisconnect, s.disconnect, r.reason);
      beginTeardown(s, nowMs);
    }
    return true;
  }
  case kWmiEvtVdevStopped: {
    uint32_t vdev = 0;
    if (!assocParseVdevStopped(vdev, payload, length) || vdev != s.options.vdevId) return false;
    s.vdevStopped = true;
    return true;
  }
  case kWmiEvtPeerDeleteResp: {
    uint32_t vdev = 0;
    uint8_t peer[6] = {};
    if (!assocParsePeerDeleteResp(vdev, peer, payload, length) || vdev != s.options.vdevId) return false;
    if (s.choice.found && sameMac(peer, s.bssid)) s.peerDeleted = true;
    return true;
  }
  case kWmiEvtScan: {
    WmiScanEvent ev;
    if (!wmiParseScanEvent(ev, payload, length) || !s.scanAsked || ev.scanId != s.scanId) return false;
    if (ev.type & (kWmiScanEvCompleted | kWmiScanEvDequeued | kWmiScanEvStartFailed)) s.scanDone = true;
    return true;
  }
  default:
    return false;
  }
}

// One turn: the step's own progress, its deadline, the next command into the outbox. Returns the step.
inline int staRun(Sta &s, uint64_t nowMs)
{
  using namespace stadetail;
  if (s.step == kStaIdle || staFinished(s)) return s.step;
  if (s.outLength) {
    // a command is still waiting for the transport; only a step's deadline moves the machine meanwhile
    if (nowMs > s.deadline + s.options.stepMs && s.step != kStaTeardown) fail(s, kStaErrTimeout, nowMs);
    return s.step;
  }
  const bool paced = nowMs >= s.pace;
  switch (s.step) {
  case kStaProfileWait:
    if (s.profileSet) {
      enter(s, kStaChoose, nowMs, s.options.stepMs);
      break;
    }
    if (nowMs > s.deadline) fail(s, kStaErrNoProfile, nowMs);
    break;
  case kStaChoose: {
    s.choice = choose(s);
    if (!s.choice.found) {
      if (s.rescansDone < s.options.rescans) {
        // a scan of our own, every channel of the regulatory list, then the choice again
        WmiScanRequest req;
        req.scanId = kAssocScanId + s.rescansDone;
        req.requestorId = kWmiScanRequestorId;
        req.vdevId = s.options.vdevId;
        req.passive = true;
        if (s.env.chanList && s.env.chanList->count) {
          req.channelCount = 0;
          for (uint32_t i = 0; i < s.env.chanList->count && i < sizeof req.channels / sizeof req.channels[0]; ++i)
            req.channels[req.channelCount++] = s.env.chanList->entry[i].freq;
        } else {
          wmiScanChannels(req, true);
        }
        s.scanId = req.scanId;
        s.scanAsked = true;
        s.scanDone = false;
        ++s.rescansDone;
        enter(s, kStaRescan, nowMs, s.options.scanWaitMs);
        if (!post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return wmiBuildStartScan(w, b, c, req); }))
          fail(s, kStaErrBuild, nowMs);
        break;
      }
      fail(s, kStaErrNotFound, nowMs);
      break;
    }
    for (unsigned i = 0; i < 6; ++i) s.bssid[i] = s.choice.net.bssid[i];
    say(s, kStaRepChosen, s.choice.index, s.choice.candidates);
    if (!securityFits(s)) {
      fail(s, kStaErrSecurity, nowMs);
      break;
    }
    if (s.options.flags & kStaFlagNoJoin) {
      s.step = kStaDone;
      if (s.note) s.note(s.ctx, kStaDone, "done (piano-wifi-l7 bit 7: the network was chosen, nothing joined)");
      break;
    }
    if (!s.env.ownMac || !s.env.frame[kStaFrameAuth].va || !s.env.frame[kStaFrameAssoc].va ||
        s.env.frame[kStaFrameAssoc].bytes < 256 ||
        ((s.options.flags & kStaFlagPeerData) && (!s.env.reoq.va || s.env.reoq.bytes < kStaReoqBytes))) {
      fail(s, kStaErrMemory, nowMs);
      break;
    }
    if (s.options.flags & kStaFlagParams) {
      s.paramsSent = 0;
      enter(s, kStaParams, nowMs, s.options.stepMs * 2);
    } else {
      enter(s, kStaPeerCreate, nowMs, s.options.stepMs);
    }
    break;
  }
  case kStaRescan:
    if (s.scanDone || nowMs > s.deadline) {
      say(s, kStaRepScanDone, s.rescansDone, s.env.scan ? s.env.scan->count : 0);
      s.scanAsked = false;
      enter(s, kStaChoose, nowMs, s.options.stepMs);
    }
    break;
  case kStaParams:
    if (!paced) break;
    if (s.paramsSent < kStaParamCount) {
      if (!postParam(s, s.paramsSent)) fail(s, kStaErrBuild, nowMs);
      else ++s.paramsSent;
      break;
    }
    enter(s, kStaPeerCreate, nowMs, s.options.stepMs);
    break;
  case kStaPeerCreate:
    if (!paced) break;
    if (!s.peerCreated) {
      const uint32_t vdev = s.options.vdevId;
      const uint8_t *peer = s.bssid;
      if (!post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildPeerCreate(w, b, c, vdev, peer); })) {
        fail(s, kStaErrBuild, nowMs);
        break;
      }
      s.peerCreated = true;   // from here the teardown deletes it
      break;
    }
    if ((s.options.flags & kStaFlagPeerMap) && s.env.htt) enter(s, kStaPeerMap, nowMs, s.options.stepMs);
    else if (s.options.flags & kStaFlagPeerData) enter(s, kStaPeerData, nowMs, s.options.stepMs * 3);
    else enter(s, kStaVdevStart, nowMs, s.options.stepMs);
    break;
  case kStaPeerMap: {
    const HttPeerMap *p = s.env.htt ? httPeerMapOf(*s.env.htt, s.options.vdevId, s.bssid) : nullptr;
    if (p || nowMs > s.deadline) {
      if (p) {
        s.peerMapped = true;
        s.peerId = p->peerId;
        s.hwPeerId = p->hwPeerId;
        say(s, kStaRepPeerMap, p->peerId, p->hwPeerId);
      } else {
        say(s, kStaRepPeerMap, 0xffffffffu, 0xffffffffu);   // not named in time: the join goes on (the log says so)
      }
      if (s.options.flags & kStaFlagPeerData) enter(s, kStaPeerData, nowMs, s.options.stepMs * 3);
      else enter(s, kStaVdevStart, nowMs, s.options.stepMs);
    }
    break;
  }
  case kStaPeerData: {
    if (!paced) break;
    const uint32_t vdev = s.options.vdevId;
    const uint8_t *peer = s.bssid;
    if (!s.routingSent) {
      const uint32_t value = kDpRxHashEnable | kDpReoDestRing << 1;
      if (!post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) {
            return assocBuildPeerSetParam(w, b, c, vdev, peer, kWmiPeerParamDefaultRouting, value);
          }))
        fail(s, kStaErrBuild, nowMs);
      s.routingSent = true;
      break;
    }
    if (s.tidsSent < kStaTids) {
      const uint32_t tid = s.tidsSent;
      uint8_t *q = s.env.reoq.va + reoqOffset(tid);
      reoQueueInit(q, tid, 1, 0);
      const uint64_t bus = s.env.reoq.bus + reoqOffset(tid);
      if (!post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) {
            return assocBuildReorderQueueSetup(w, b, c, vdev, peer, tid, bus, 1);
          }))
        fail(s, kStaErrBuild, nowMs);
      ++s.tidsSent;
      break;
    }
    enter(s, kStaVdevStart, nowMs, s.options.stepMs);
    break;
  }
  case kStaVdevStart:
    if (!s.vdevStartSent) {
      if (!paced) break;
      if (!postVdevStart(s)) {
        fail(s, kStaErrBuild, nowMs);
        break;
      }
      s.vdevStartSent = true;   // from here the teardown stops the device
      s.deadline = nowMs + s.options.stepMs;
      break;
    }
    if (nowMs > s.deadline) fail(s, kStaErrTimeout, nowMs);
    break;
  case kStaAuth:
  case kStaAssoc: {
    const bool auth = s.step == kStaAuth;
    if (!s.frameOut) {
      if (!paced) break;
      if (s.tries >= s.options.frameTries) {
        fail(s, kStaErrNoAnswer, nowMs);
        break;
      }
      ++s.tries;
      say(s, kStaRepTry, uint32_t(s.step), s.tries);
      if (!(auth ? postAuth(s) : postAssoc(s))) {
        fail(s, kStaErrBuild, nowMs);
        break;
      }
      s.frameOut = true;
      s.deadline = nowMs + (auth ? s.options.frameWaitMs : s.options.frameWaitMs * 2);
      break;
    }
    if (nowMs > s.deadline) s.frameOut = false;   // no answer in time: the next try
    break;
  }
  case kStaPeerAssoc:
    if (s.peerAssocConf || nowMs > s.deadline) {
      if (!s.peerAssocConf) say(s, kStaRepPeerAssocConf, 0, uint32_t(nowMs - s.sentAt));
      enter(s, kStaVdevUp, nowMs, s.options.stepMs);
      const uint32_t vdev = s.options.vdevId, aid = s.aid;
      const uint8_t *bssid = s.bssid;
      if (!post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildVdevUp(w, b, c, vdev, aid, bssid); }))
        fail(s, kStaErrBuild, nowMs);
    }
    break;
  case kStaVdevUp:
    // the command went out (the outbox is empty): the station is up
    s.vdevUp = true;
    s.associatedAt = nowMs;
    if (!s.rsn) {
      enter(s, kStaAuthorize, nowMs, s.options.stepMs);
      const uint32_t vdev = s.options.vdevId;
      const uint8_t *peer = s.bssid;
      if (!post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) {
            return assocBuildPeerSetParam(w, b, c, vdev, peer, kWmiPeerParamAuthorize, 1);
          }))
        fail(s, kStaErrBuild, nowMs);
      break;
    }
    enter(s, kStaAssociated, nowMs, s.options.holdMs);
    break;
  case kStaAuthorize:
    s.authorized = true;
    enter(s, kStaAssociated, nowMs, s.options.holdMs);
    break;
  case kStaAssociated:
    if (s.options.holdMs && nowMs > s.deadline) beginTeardown(s, nowMs);
    break;
  case kStaTeardown: {
    // In order, each piece only when what it undoes was done: our deauthentication (and a moment for it to leave), the station
    // down, the peer deleted (its answer waited for, bounded), the device stopped (the same). A piece the firmware does not
    // answer is reported and the teardown goes on.
    const uint32_t vdev = s.options.vdevId;
    const uint8_t *peer = s.bssid;
    if (!paced || (s.teardownWait && nowMs < s.teardownWait && !(s.tdPhase == 4 && s.peerDeleted) &&
                   !(s.tdPhase == 6 && s.vdevStopped)))
      break;
    switch (s.tdPhase) {
    case 0:
      s.tdPhase = 1;
      if (s.vdevUp && !s.disconnect && (s.options.flags & kStaFlagDeauth) && s.env.frame[kStaFrameDeauth].va) {
        const StaDma &d = s.env.frame[kStaFrameDeauth];
        const uint32_t length = dot11BuildDeauth(d.va, d.bytes, s.env.ownMac, s.bssid, ++s.seq, kDot11ReasonLeaving);
        if (length && postFrame(s, kStaFrameDeauth, length)) {
          s.deauthSent = true;
          s.teardownWait = nowMs + 150;   // the frame leaves before the station goes down
          say(s, kStaRepTeardown, kStaTdDeauth, 1);
        }
      }
      break;
    case 1:
      s.tdPhase = 2;
      s.teardownWait = 0;
      if (s.vdevUp && post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildVdevDown(w, b, c, vdev); })) {
        s.downSent = true;
        say(s, kStaRepTeardown, kStaTdDown, 1);
      }
      break;
    case 2:
      s.tdPhase = 4;   // the peer's answer is waited for in phase 4
      if (s.peerCreated && post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildPeerDelete(w, b, c, vdev, peer); })) {
        s.peerDeleteSent = true;
        s.teardownWait = nowMs + s.options.stepMs;
      }
      break;
    case 4:
      if (s.peerDeleteSent) say(s, kStaRepTeardown, kStaTdPeerDelete, s.peerDeleted ? 1 : 0);
      s.teardownWait = 0;
      s.tdPhase = 6;   // the device's answer is waited for in phase 6
      if (s.vdevStartSent && post(s, [&](WmiWriter &w, uint8_t *b, uint32_t c) { return assocBuildVdevStop(w, b, c, vdev); })) {
        s.stopSent = true;
        s.teardownWait = nowMs + s.options.stepMs;
      }
      break;
    default:
      if (s.stopSent) say(s, kStaRepTeardown, kStaTdStop, s.vdevStopped ? 1 : 0);
      s.teardownWait = 0;
      s.step = s.error ? kStaStopped : kStaDone;
      if (s.note) s.note(s.ctx, s.step, staStepName(s.step));
      break;
    }
    break;
  }
  default:
    break;
  }
  // a deadline of a step that waits for an answer (the start response and the frames have their own handling above)
  if (!staFinished(s) && s.step != kStaTeardown && s.step != kStaAssociated && s.step != kStaAuth && s.step != kStaAssoc &&
      s.step != kStaVdevStart && s.step != kStaProfileWait && s.step != kStaRescan && s.step != kStaPeerMap &&
      s.step != kStaPeerAssoc && nowMs > s.deadline + s.options.stepMs)
    fail(s, kStaErrTimeout, nowMs);
  return s.step;
}

}  // namespace pianowifi
