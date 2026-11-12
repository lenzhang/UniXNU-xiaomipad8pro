// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 6, the ladder: the copy engine (WifiCe.h), the host transport (WifiHtc.h) and the firmware's message interface
// (WifiWmi.h) as one step machine the owner steps with a clock, so that a host test drives exactly what the kext drives (tests/
// wmi_host.cpp, against a model of the copy engine and a firmware that speaks the transport and the message interface). New code
// for this repository.
//
// The steps, each with its own deadline (piano-wifi-wmitimeout, and piano-wifi-scanwait for the scan itself):
//   601 the transport's ready       <- the firmware speaks first: its credits and its credit size
//   602 the message interface       -> connect that service, and take the endpoint its answer gives
//   603 set-up complete             -> the handshake's last control message
//   604 the chip's capability       <- the service-ready event (radio chains, bands, how much memory it wants: none here) and
//                                      the one or two extended ones behind it, which this firmware's own host waits for too
//   605 the host's initialisation   -> the resource configuration
//   606 the firmware is ready       <- the ready event, with the chip's own MAC address
//   607 a station virtual device    -> created, never started (a scan needs no started device on this chip)
//   608 a passive scan              -> the channel list of WifiHw.h
//   609 the scan's results          <- started, one management frame per beacon or probe answer, completed
//   610 the teardown                -> the scan stopped if it still runs, the virtual device deleted, the rings disabled
// Level 6 ends there: no association, no keys and no data path (levels 7 and 8).
//
// Every command the machine wants to send is written down as the one command outstanding and sent by the next turn, because a
// command of the firmware's message interface costs credits and a turn may find none left: a credit wait is not a failure, and
// the step's deadline is what decides. Nothing here allocates, maps or sleeps.
#pragma once
#include <stdint.h>
#include "WifiAssoc.h"
#include "WifiCe.h"
#include "WifiHtc.h"
#include "WifiWmi.h"

namespace pianowifi {

enum LinkStep : int {
  kLinkIdle = 600, kLinkTransport = 601, kLinkConnect = 602, kLinkSetup = 603, kLinkCapability = 604, kLinkInit = 605,
  kLinkReadyWait = 606, kLinkVdev = 607, kLinkScanStart = 608, kLinkScanning = 609, kLinkTeardown = 610, kLinkDone = 611,
  kLinkStopped = 612,
  // The radio's set-up between the firmware's ready event and the virtual device (P22, docs §10.27), each behind a bit of
  // piano-wifi-l6pre. They run in this order, and their numbers are above the ladder's so that 601-612 keep their meaning.
  kLinkHwMode = 620, kLinkPdevParams = 621, kLinkRegWait = 622, kLinkChanList = 623, kLinkHold = 624,
};

enum LinkError : int {
  kLinkErrNone = 0, kLinkErrNoBuffer = -1, kLinkErrSend = -2, kLinkErrMessage = -3, kLinkErrTransport = -4,
  kLinkErrRefused = -5, kLinkErrMemory = -6, kLinkErrScan = -7, kLinkErrTimeout = -8, kLinkErrState = -9, kLinkErrBuild = -10,
};

// What the machine still has to send. One at a time: the firmware answers each before the next is due.
enum LinkPending : uint32_t {
  kLinkSendNothing = 0, kLinkSendConnect, kLinkSendSetup, kLinkSendInit, kLinkSendVdev, kLinkSendScan, kLinkSendStopScan,
  kLinkSendDeleteVdev, kLinkSendHwMode, kLinkSendPdevParam, kLinkSendChanList,
};

inline const char *linkStepName(int step)
{
  switch (step) {
  case kLinkIdle: return "idle";
  case kLinkTransport: return "waiting for the transport's ready";
  case kLinkConnect: return "connecting the firmware's message interface";
  case kLinkSetup: return "the transport's set-up complete";
  case kLinkCapability: return "waiting for the chip's capability";
  case kLinkInit: return "the host's initialisation";
  case kLinkReadyWait: return "waiting for the firmware to be ready";
  case kLinkVdev: return "a station virtual device";
  case kLinkScanStart: return "a passive scan";
  case kLinkScanning: return "the scan's results";
  case kLinkTeardown: return "the teardown";
  case kLinkDone: return "done";
  case kLinkHwMode: return "the radio's hardware mode";
  case kLinkPdevParams: return "the radio's parameters";
  case kLinkRegWait: return "waiting for the regulatory event";
  case kLinkChanList: return "the channel list";
  case kLinkHold: return "holding after the virtual device (piano-wifi-l6pre bit 6)";
  default: return "stopped";
  }
}

inline const char *linkErrorName(int error)
{
  switch (error) {
  case kLinkErrNone: return "none";
  case kLinkErrNoBuffer: return "no transport buffer was free";
  case kLinkErrSend: return "the copy engine refused the message";
  case kLinkErrMessage: return "a message did not parse";
  case kLinkErrTransport: return "the transport's handshake did not work";
  case kLinkErrRefused: return "the firmware refused the service";
  case kLinkErrMemory: return "the firmware asked for memory this level does not hand out";
  case kLinkErrScan: return "the scan did not run";
  case kLinkErrTimeout: return "a step timed out";
  case kLinkErrState: return "a message arrived in the wrong step";
  default: return "a message could not be built";
  }
}

struct LinkOptions {
  uint32_t timeoutMs = kWmiTimeoutMs;
  uint32_t scanWaitMs = kScanWaitMsDefault;
  uint32_t vdevId = 0;
  bool with5GHz = true;
  bool passive = true;
  uint32_t settleMs = 100;        // between the virtual device and the scan, as this firmware's own host leaves it
  uint32_t extWaitMs = 500;       // after the chip's capability, for the extended ones that follow it (step 604)
  // The radio's set-up before the virtual device (P22, §10.27): piano-wifi-l6pre and its three times.
  uint32_t pre = kPreDefault;
  uint32_t regWaitMs = kPreRegWaitMsDefault;
  uint32_t paceMs = kPrePaceMsDefault;
  uint32_t holdMs = kPreHoldMsDefault;
};

struct Link {
  Ce *ce = nullptr;
  ChipIo io;
  Htc htc;
  LinkOptions options;
  WmiServiceReady caps;
  WmiReady ready;
  WmiScanRequest request;
  ScanList scan;
  int step = kLinkIdle, error = kLinkErrNone, failedStep = 0;
  int endpoint = -1;                               // the endpoint the firmware's message interface got
  uint32_t pending = kLinkSendNothing;
  uint64_t deadline = 0, settle = 0, extSettle = 0, began = 0;
  bool ext2Seen = false;          // the second extended capability event: the last the firmware sends before it waits
  bool scanStarted = false, scanCompleted = false, scanTimedOut = false, stopped = false, deleted = false;
  uint32_t scanReason = 0, scanChannels = 0, scanEventsSeen = 0;
  // the radio's set-up (P22, §10.27)
  RegDomain reg;
  ChanList chanList;
  uint32_t paramsSent = 0, hwModeStatus = 0, chanListChannels = 0;
  bool hwModeSent = false, hwModeAnswered = false, chanListSent = false, chanListFromEvent = false;
  uint64_t pace = 0, hold = 0, regDeadline = 0;
  uint8_t vdevMac[6] = {};
  const char *vdevMacFrom = "the firmware's ready event";
  // counters for the log
  uint32_t commands = 0, events = 0, unexpected = 0, mgmtFrames = 0, controlMessages = 0, creditWaits = 0,
           badMessages = 0, extEvents = 0;
  uint32_t lastEvent = 0;
  // every event this level knows nothing about, so that a log names them instead of a count alone
  uint32_t unknownEvent[8] = {};
  uint32_t unknownEvents = 0;
  int lastCommandStep = 0;        // the step whose command went out last: what a firmware death comes after
  void (*note)(void *ctx, int step, const char *what) = nullptr;
  void *noteCtx = nullptr;
};

namespace linkdetail {

// The chain mask the resource configuration and a virtual device take, out of the chain count the firmware reported: a count
// that is not between one and eight is not believed (it would shift a word off its end), and the chip's two chains are what
// the mask falls back to.
inline uint32_t chainMaskOf(uint32_t chains)
{
  if (!chains || chains > 8) return 0x3u;
  return (1u << chains) - 1u;
}

inline uint32_t chainCountOf(uint32_t chains) { return (!chains || chains > 8) ? 2u : chains; }

inline int fail(Link &link, int error)
{
  link.failedStep = link.step;
  link.error = error;
  link.step = kLinkStopped;
  link.pending = kLinkSendNothing;
  return link.error;
}

inline void enter(Link &link, int step, uint64_t nowMs)
{
  link.step = step;
  link.deadline = nowMs + (step == kLinkScanning ? link.options.scanWaitMs
                           : step == kLinkRegWait ? link.options.regWaitMs + link.options.timeoutMs
                           : step == kLinkHold    ? link.options.holdMs + link.options.timeoutMs
                                                  : link.options.timeoutMs);
  if (link.note) link.note(link.noteCtx, step, linkStepName(step));
}

// The address the virtual device is created with: the firmware's own when it reports one, then the address its ready event
// carries behind it, and last — only with piano-wifi-l6pre bit 3 — a locally administered one of piano's own. A virtual
// device with an all-zero address is what this firmware's own host never creates (P22, §10.27). The address itself is never
// logged here: only where it came from.
inline void chooseVdevMac(Link &link)
{
  const uint8_t *from = nullptr;
  if (wmiMacUsable(link.ready.mac)) {
    from = link.ready.mac;
    link.vdevMacFrom = "the firmware's ready event";
  } else if (link.ready.extraMacSeen) {
    from = link.ready.extraMac;
    link.vdevMacFrom = "the address behind the ready event";
  } else if (link.options.pre & kPreVdevMac) {
    from = kLocalMac;
    link.vdevMacFrom = "a locally administered address of piano's own (the firmware reported none)";
  } else {
    link.vdevMacFrom = "the firmware's ready event (all zeros: piano-wifi-l6pre bit 3 is off)";
  }
  for (unsigned i = 0; i < 6; ++i) link.vdevMac[i] = from ? from[i] : 0;
}

// Where the ladder goes after the firmware's ready event: the first of the radio's set-up steps the options ask for, and the
// virtual device when they ask for none.
inline void afterReady(Link &link, uint64_t nowMs);
inline void afterPre(Link &link, int from, uint64_t nowMs);

// The pipes a service uses are not negotiated: they are the entries level 5 sent the firmware (WifiHw.h kCeServices).
inline bool pipesOf(uint32_t service, uint8_t &ulPipe, uint8_t &dlPipe)
{
  bool haveOut = false, haveIn = false;
  for (const CeServiceConfig &entry : kCeServices) {
    if (entry.service != service) continue;
    if (entry.dir == kCeDirOut) {
      ulPipe = uint8_t(entry.pipe);
      haveOut = true;
    } else if (entry.dir == kCeDirIn) {
      dlPipe = uint8_t(entry.pipe);
      haveIn = true;
    }
  }
  return haveOut && haveIn;
}

// One message out on an endpoint: a claimed buffer, the message built after the transport's header, the credits taken (an
// endpoint under credit flow control), the header written and one descriptor on the endpoint's outbound pipe.
// Returns 1 sent, 0 "not now" (no credits, no buffer of ours free), -1 failed for good.
template <class Build>
inline int send(Link &link, uint8_t endpoint, Build build)
{
  if (endpoint >= kHtcEndpoints || !link.htc.ep[endpoint].used) return fail(link, kLinkErrState), -1;
  const int buffer = ceClaim(*link.ce);
  if (buffer < 0) {
    ++link.creditWaits;
    return 0;                      // every send buffer is in flight: the next turn frees one
  }
  uint8_t *va = link.ce->tx[buffer].va;
  const uint32_t capacity = link.ce->tx[buffer].bytes - kHtcHeaderBytes;
  const uint32_t payload = build(va + kHtcHeaderBytes, capacity);
  if (!payload) {
    ceRelease(*link.ce, uint32_t(buffer));
    return fail(link, kLinkErrBuild), -1;
  }
  if (!htcTakeCredits(link.htc, endpoint, payload + kHtcHeaderBytes)) {
    ceRelease(*link.ce, uint32_t(buffer));
    ++link.creditWaits;
    return 0;                      // the firmware gives credits back in the trailer of what it sends
  }
  const uint32_t total = htcPrepare(link.htc, endpoint, va, payload);
  const int pipe = ceIndexOf(link.htc.ep[endpoint].ulPipe);
  // The descriptor's own identifier is the endpoint, which is what the firmware's host puts there and what comes back in the
  // completion's status entry.
  if (pipe < 0 || !ceSend(*link.ce, link.io, unsigned(pipe), unsigned(buffer), total, endpoint)) {
    ceRelease(*link.ce, uint32_t(buffer));
    return fail(link, kLinkErrSend), -1;
  }
  if (endpoint == 0) ++link.controlMessages;
  else ++link.commands;
  return 1;
}

// The one command outstanding, if the turn can send it. Returns true when it went out.
inline bool sendPending(Link &link, uint64_t nowMs)
{
  const WmiScanRequest request = link.request;
  const uint32_t vdevId = link.options.vdevId;
  int rc = 0;
  switch (link.pending) {
  case kLinkSendConnect: {
    const uint32_t credits = htcAllocatable(link.htc.credits);   // the connection flags carry one byte of them
    rc = send(link, 0, [&](uint8_t *at, uint32_t capacity) {
      return htcBuildConnect(at, capacity, uint16_t(kSvcWmiControl), credits, true);
    });
    break;
  }
  case kLinkSendSetup:
    rc = send(link, 0, [](uint8_t *at, uint32_t capacity) { return htcBuildSetupComplete(at, capacity); });
    if (rc == 1) enter(link, kLinkCapability, nowMs);
    break;
  case kLinkSendInit: {
    const uint32_t chains = chainMaskOf(link.caps.rfChains);
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return wmiBuildInit(w, at, capacity, chains);
    });
    if (rc == 1) enter(link, kLinkReadyWait, nowMs);
    break;
  }
  case kLinkSendVdev: {
    const uint32_t chains = chainCountOf(link.caps.rfChains);
    const uint8_t *mac = link.vdevMac;
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return wmiBuildVdevCreate(w, at, capacity, vdevId, mac, kWmiPdevIdSoc, chains);
    });
    // A virtual device is answered by no event of its own: the scan follows a settle time later.
    if (rc == 1) link.settle = nowMs + link.options.settleMs;
    break;
  }
  case kLinkSendHwMode:
    rc = send(link, uint8_t(link.endpoint), [](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return assocBuildSetHwMode(w, at, capacity, kWmiHwModeDbs);
    });
    if (rc == 1) {
      link.hwModeSent = true;
      link.pace = nowMs + link.options.paceMs;
    }
    break;
  case kLinkSendPdevParam: {
    PdevParam plan[kPdevParamCount];
    const uint32_t count = pdevParamPlan(plan, kPdevParamCount, chainMaskOf(link.caps.rfChains));
    if (link.paramsSent >= count) {
      link.pending = kLinkSendNothing;
      return false;
    }
    const PdevParam one = plan[link.paramsSent];
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return assocBuildPdevSetParam(w, at, capacity, one.param, one.value);
    });
    if (rc == 1) {
      ++link.paramsSent;
      link.pace = nowMs + link.options.paceMs;
      if (link.paramsSent < count) {
        link.pending = kLinkSendPdevParam;   // the next one on a later turn
        link.lastCommandStep = link.step;
        return true;
      }
    }
    break;
  }
  case kLinkSendChanList: {
    const ChanList list = link.chanList;
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return assocBuildChanList(w, at, capacity, list, false);
    });
    if (rc == 1) {
      link.chanListSent = true;
      link.pace = nowMs + link.options.paceMs;
    }
    break;
  }
  case kLinkSendScan:
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return wmiBuildStartScan(w, at, capacity, request);
    });
    if (rc == 1) enter(link, kLinkScanning, nowMs);
    break;
  case kLinkSendStopScan:
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return wmiBuildStopScan(w, at, capacity, request);
    });
    if (rc == 1) link.stopped = true;
    break;
  case kLinkSendDeleteVdev:
    rc = send(link, uint8_t(link.endpoint), [&](uint8_t *at, uint32_t capacity) {
      WmiWriter w;
      return wmiBuildVdevDelete(w, at, capacity, vdevId);
    });
    if (rc == 1) link.deleted = true;
    break;
  default:
    return false;
  }
  if (rc == 1) {
    link.pending = kLinkSendNothing;
    link.lastCommandStep = link.step;
  }
  return rc == 1;
}

// The order of the radio's set-up: the hardware mode, the parameters, the regulatory event, the channel list, and then the
// virtual device (or the hold, with piano-wifi-l6pre bit 6). Each piece is skipped when its bit is off.
inline void afterPre(Link &link, int from, uint64_t nowMs)
{
  const uint32_t pre = link.options.pre;
  link.pace = 0;
  if (from < kLinkPdevParams && (pre & kPrePdevParams)) {
    enter(link, kLinkPdevParams, nowMs);
    link.pending = kLinkSendPdevParam;
    return;
  }
  if (from < kLinkRegWait && (pre & kPreChanList) && (pre & kPreRegWait) && !link.reg.seen) {
    enter(link, kLinkRegWait, nowMs);
    link.regDeadline = nowMs + link.options.regWaitMs;
    return;
  }
  if (from < kLinkChanList && (pre & kPreChanList)) {
    // The firmware's own domain when its event arrived, and the static list of WifiHw.h when it did not.
    if (link.reg.seen) chanListBuild(link.chanList, link.reg, link.options.with5GHz);
    if (link.reg.seen && link.chanList.count) link.chanListFromEvent = true;
    else chanListFallback(link.chanList, link.options.with5GHz);
    link.chanListChannels = link.chanList.count;
    enter(link, kLinkChanList, nowMs);
    link.pending = kLinkSendChanList;
    return;
  }
  chooseVdevMac(link);
  enter(link, kLinkVdev, nowMs);
  link.pending = kLinkSendVdev;
}

inline void afterReady(Link &link, uint64_t nowMs)
{
  if (link.options.pre & kPreHwMode) {
    enter(link, kLinkHwMode, nowMs);
    link.pending = kLinkSendHwMode;
    return;
  }
  afterPre(link, kLinkHwMode, nowMs);
}

inline void onControl(Link &link, const HtcRx &rx, uint64_t nowMs)
{
  switch (rx.message) {
  case kHtcMsgReady: {
    if (link.step != kLinkTransport) {
      ++link.unexpected;
      return;
    }
    if (!htcParseReady(link.htc, rx.payload, rx.length)) {
      fail(link, kLinkErrTransport);
      return;
    }
    uint8_t ul = 0, dl = 0;
    if (!pipesOf(kSvcWmiControl, ul, dl)) {
      fail(link, kLinkErrState);
      return;
    }
    enter(link, kLinkConnect, nowMs);
    link.pending = kLinkSendConnect;
    return;
  }
  case kHtcMsgConnectServiceResp: {
    if (link.step != kLinkConnect) {
      ++link.unexpected;
      return;
    }
    HtcConnect answer;
    if (!htcParseConnectResp(link.htc, rx.payload, rx.length, answer)) {
      fail(link, answer.refused ? kLinkErrRefused : kLinkErrTransport);
      return;
    }
    uint8_t ul = 0, dl = 0;
    if (!pipesOf(answer.service, ul, dl) ||
        !htcEndpointOpen(link.htc, answer, ul, dl, true, htcAllocatable(link.htc.credits))) {
      fail(link, kLinkErrTransport);
      return;
    }
    link.endpoint = int(answer.endpoint);
    enter(link, kLinkSetup, nowMs);
    link.pending = kLinkSendSetup;
    return;
  }
  default:
    ++link.unexpected;
    return;
  }
}

inline void onEvent(Link &link, const uint8_t *message, uint32_t length, uint64_t nowMs)
{
  const uint32_t id = wmiEventId(message, length);
  const uint8_t *payload = message + kWmiCmdHeaderBytes;
  const uint32_t payloadLength = length - kWmiCmdHeaderBytes;
  ++link.events;
  link.lastEvent = id;
  switch (id) {
  case kWmiEvtServiceReady:
    if (link.step != kLinkCapability) {
      ++link.unexpected;
      return;
    }
    if (!wmiParseServiceReady(link.caps, payload, payloadLength)) {
      fail(link, kLinkErrMessage);
      return;
    }
    if (link.caps.memRequests) {   // this chip's firmware asks for none; one that does is not answered at this level
      fail(link, kLinkErrMemory);
      return;
    }
    // The firmware sends one or two extended capability events after this one, and its own host sends the initialisation only
    // once they are in. The step stands until the second of them arrives or the short wait of the options runs out.
    link.extSettle = nowMs + link.options.extWaitMs;
    return;
  case kWmiEvtReady:
    if (link.step != kLinkReadyWait) {
      ++link.unexpected;
      return;
    }
    if (!wmiParseReady(link.ready, payload, payloadLength)) {
      fail(link, kLinkErrMessage);
      return;
    }
    afterReady(link, nowMs);
    return;
  case kWmiEvtScan: {
    WmiScanEvent ev;
    if (!wmiParseScanEvent(ev, payload, payloadLength)) {
      ++link.unexpected;
      return;
    }
    if (ev.scanId != link.request.scanId) {
      ++link.unexpected;
      return;
    }
    ++link.scanEventsSeen;
    if (ev.type & kWmiScanEvStarted) link.scanStarted = true;
    if (ev.type & (kWmiScanEvForeignChannel | kWmiScanEvBssChannel)) ++link.scanChannels;
    if (ev.type & kWmiScanEvStartFailed) {
      link.scanReason = ev.reason;
      fail(link, kLinkErrScan);
      return;
    }
    if (ev.type & (kWmiScanEvCompleted | kWmiScanEvDequeued)) {
      link.scanCompleted = true;
      link.scanReason = ev.reason;
      if (link.step != kLinkTeardown) {   // the teardown may already be running (it is what asked the scan to stop)
        enter(link, kLinkTeardown, nowMs);
        link.stopped = true;             // nothing to stop: the scan ended by itself
        link.pending = kLinkSendDeleteVdev;
      }
    }
    return;
  }
  case kWmiEvtMgmtRx: {
    WmiMgmtRx rx;
    if (!wmiParseMgmtRx(rx, payload, payloadLength)) {
      ++link.unexpected;
      return;
    }
    ++link.mgmtFrames;
    (void)scanAdd(link.scan, rx);
    return;
  }
  case kWmiEvtServiceReadyExt:
  case kWmiEvtServiceReadyExt2:
    ++link.extEvents;
    if (id == uint32_t(kWmiEvtServiceReadyExt2)) link.ext2Seen = true;
    return;
  case kWmiEvtRegChanListCc:
  case kWmiEvtRegChanListCcExt:
    // The firmware sends its regulatory domain by itself after the initialisation: it is what the channel list is built from
    // (P22, §10.27). A second one replaces the first, as a domain change would.
    if (!regParse(link.reg, payload, payloadLength, id == uint32_t(kWmiEvtRegChanListCcExt))) ++link.badMessages;
    return;
  case kWmiEvtPdevSetHwModeResp: {
    link.hwModeAnswered = true;
    uint32_t size = 0;
    const uint8_t *ev = wmiFind(payload, payloadLength, kWmiTagPdevSetHwModeResp, &size);
    if (ev && size >= 4) link.hwModeStatus = wmiGet(ev, 0);
    return;
  }
  case kWmiEvtServiceAvailable:
  case kWmiEvtChanInfo:
  case kWmiEvtVdevDeleteResp:
  case kWmiEvtDebugMessage:
  case kWmiEvtDiag:
    return;                     // read and counted, acted on by no step of this level
  default:
    ++link.unexpected;
    // The identifier of an event this level knows nothing about: a count alone said nothing about the three the first level 6
    // boot saw (§10.27), so the first eight are kept for the log.
    if (link.unknownEvents < sizeof link.unknownEvent / sizeof link.unknownEvent[0])
      link.unknownEvent[link.unknownEvents] = id;
    ++link.unknownEvents;
    return;
  }
}

}  // namespace linkdetail

// The ladder begins: the copy engine's rings must already run (the owner planned and programmed them, and posted the receive
// buffers), and the firmware speaks first, so this only sets the step and its deadline.
inline void linkBegin(Link &link, Ce &ce, const ChipIo &io, const LinkOptions &options, uint64_t nowMs)
{
  const auto note = link.note;
  void *const ctx = link.noteCtx;
  link = Link{};
  link.ce = &ce;
  link.io = io;
  link.options = options;
  link.note = note;
  link.noteCtx = ctx;
  link.began = nowMs;
  htcBegin(link.htc);
  link.request.scanId = kWmiScanId;
  link.request.requestorId = kWmiScanRequestorId;
  link.request.vdevId = options.vdevId;
  link.request.passive = options.passive;
  wmiScanChannels(link.request, options.with5GHz);
  linkdetail::enter(link, kLinkTransport, nowMs);
}

// One message out of a receive buffer, through the transport and into the step machine. The transport's part runs whatever step
// the machine is in, because the credits a message carries back are what lets the teardown's own commands go out.
inline void linkReceive(Link &link, const uint8_t *data, uint32_t length, uint64_t nowMs)
{
  using namespace linkdetail;
  const HtcRx rx = htcReceive(link.htc, data, length);
  if (link.step == kLinkDone || link.step == kLinkStopped) return;
  switch (rx.kind) {
  case kHtcControl:
    onControl(link, rx, nowMs);
    return;
  case kHtcPayload:
    if (int(rx.endpoint) != link.endpoint || rx.length < kWmiCmdHeaderBytes) {
      ++link.unexpected;
      return;
    }
    onEvent(link, rx.payload, rx.length, nowMs);
    return;
  case kHtcBad:
    // A message the transport cannot read is dropped and counted, never acted on and never fatal: this firmware's own host
    // does the same, and a link that only ever sends rubbish runs into the step's deadline anyway.
    ++link.badMessages;
    return;
  default:
    return;
  }
}

// One turn of the ladder: the copy engine's events, the buffers given back, the one command outstanding if it can go out, the
// steps that move by themselves (the scan after its settle time, the teardown), then the step's deadline. Returns the step.
inline int linkRun(Link &link, uint64_t nowMs)
{
  using namespace linkdetail;
  if (!link.ce || link.step == kLinkDone || link.step == kLinkStopped) return link.step;
  cePoll(*link.ce, link.io, [&](uint32_t kind, uint32_t, uint8_t *va, uint32_t length, uint32_t) {
    if (kind == kCeReceived) linkReceive(link, va, length, nowMs);
  });
  for (unsigned i = 0; i < kCeHostPipeCount; ++i) cePostRx(*link.ce, link.io, i);
  if (link.step == kLinkDone || link.step == kLinkStopped) return link.step;
  if (link.pending != kLinkSendNothing) {
    sendPending(link, nowMs);
    if (link.step == kLinkStopped) return link.step;
  }
  // The initialisation follows the chip's capability once its extended events are in (or the short wait is over).
  if (link.step == kLinkCapability && link.caps.seen && link.pending == kLinkSendNothing && link.extSettle &&
      (link.ext2Seen || nowMs >= link.extSettle)) {
    link.extSettle = 0;
    enter(link, kLinkInit, nowMs);
    link.pending = kLinkSendInit;
    sendPending(link, nowMs);
    return link.step;
  }
  // The radio's set-up: each piece moves on once its command is out and the pace time has passed (with piano-wifi-l6pre bit
  // 5; without it the next turn goes straight on, as before).
  if (link.pending == kLinkSendNothing &&
      (link.step == kLinkHwMode || link.step == kLinkPdevParams || link.step == kLinkChanList)) {
    const bool paced = !(link.options.pre & kPrePace) || !link.pace || nowMs >= link.pace;
    if (paced) {
      afterPre(link, link.step, nowMs);
      sendPending(link, nowMs);
      return link.step;
    }
  }
  // The regulatory event, or the wait for it running out.
  if (link.step == kLinkRegWait && link.pending == kLinkSendNothing && (link.reg.seen || nowMs >= link.regDeadline)) {
    afterPre(link, kLinkRegWait, nowMs);
    sendPending(link, nowMs);
    return link.step;
  }
  // The scan follows the virtual device once the settle time has passed — or, with piano-wifi-l6pre bit 6, the level holds
  // there instead, so that a firmware death after the virtual device cannot be blamed on the scan.
  if (link.step == kLinkVdev && link.pending == kLinkSendNothing && link.settle && nowMs >= link.settle) {
    link.settle = 0;
    if (link.options.pre & kPreNoScan) {
      enter(link, kLinkHold, nowMs);
      link.hold = nowMs + link.options.holdMs;
      return link.step;
    }
    enter(link, kLinkScanStart, nowMs);
    link.pending = kLinkSendScan;
    sendPending(link, nowMs);
    return link.step;
  }
  // The hold's end: the teardown, as a completed scan reaches it.
  if (link.step == kLinkHold && nowMs >= link.hold) {
    enter(link, kLinkTeardown, nowMs);
    link.stopped = true;   // no scan ran
    link.pending = kLinkSendDeleteVdev;
    sendPending(link, nowMs);
    return link.step;
  }
  if (link.step == kLinkTeardown && link.pending == kLinkSendNothing) {
    if (!link.stopped) {
      link.pending = kLinkSendStopScan;
      sendPending(link, nowMs);
      return link.step;
    }
    if (!link.deleted) {
      link.pending = kLinkSendDeleteVdev;
      sendPending(link, nowMs);
      return link.step;
    }
    link.step = kLinkDone;
    if (link.note) link.note(link.noteCtx, kLinkDone, linkStepName(kLinkDone));
    return link.step;
  }
  if (nowMs > link.deadline) {
    // A scan that started and then stopped answering is not a failure of the level: what it found stands and the teardown
    // runs. Anything else at a deadline is.
    if (link.step == kLinkScanning && link.scanStarted) {
      link.scanTimedOut = true;
      enter(link, kLinkTeardown, nowMs);
      return link.step;
    }
    return fail(link, kLinkErrTimeout);
  }
  return link.step;
}

// What the owner calls when it gives up on the ladder, or when the radio must stop (a suspend): the scan stopped and the
// virtual device deleted. One turn each, so that the owner's loop stays bounded; true when there is nothing left to send and
// the chip has taken everything that was sent — only then may the rings be disabled, because a ring disabled with a descriptor
// the chip has not read yet loses that message.
inline bool linkStopRequest(Link &link, uint64_t nowMs)
{
  using namespace linkdetail;
  if (!link.ce) return true;
  cePoll(*link.ce, link.io, [&](uint32_t kind, uint32_t, uint8_t *va, uint32_t length, uint32_t) {
    if (kind == kCeReceived) linkReceive(link, va, length, nowMs);
  });
  for (unsigned i = 0; i < kCeHostPipeCount; ++i) cePostRx(*link.ce, link.io, i);
  if (link.endpoint > 0) {
    if (link.scanStarted && !link.scanCompleted && !link.stopped) {
      link.pending = kLinkSendStopScan;
      sendPending(link, nowMs);
      return false;
    }
    if (link.ready.seen && !link.deleted) {
      link.pending = kLinkSendDeleteVdev;
      sendPending(link, nowMs);
      return false;
    }
  }
  link.pending = kLinkSendNothing;
  return ceIdle(*link.ce);
}

// The rings disabled, after linkStopRequest said there is nothing in flight (or after the owner's bounded wait gave up on it).
// The window itself is never freed (wifi.md section 10.10): only a reset guarantees the chip has stopped reading it.
inline void linkDisable(Link &link)
{
  if (!link.ce) return;
  link.pending = kLinkSendNothing;
  ceStop(*link.ce, link.io);
}

inline bool linkFinished(const Link &link) { return link.step == kLinkDone; }

}  // namespace pianowifi
