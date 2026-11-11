// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoUsbHost's UCSI client (docs/drivers/usb-host.md H4): the OS policy manager's side of UCSI over the DSP's owner 0x800b, through
// PianoPower's USB-C relay (package/include/PianoUsbcContract.h). Portable: no I/O, no waits, no allocation; the kext hands in each
// message and the time, sends what take() gives, and calls tick() at deadline(). Host-tested against a simulated PPM
// (tests/ucsi_host.cpp). The data structures are Ucsi.h's.
//
// What it does, in Linux's order (ucsi.c ucsi_init / ucsi_reset_ppm / ucsi_run_command / ucsi_handle_connector_change and
// ucsi_glink.c, read as described behaviour):
//   init   a read of the mailbox (VERSION, CCI); a reset completion still latched is cleared first (SET_NOTIFICATION_ENABLE with no
//          bit, its completion polled); PPM_RESET, the CCI polled every 20 ms until RESET_COMPLETE (re-sent while the CCI shows
//          anything else; notifications are off after a reset); SET_NOTIFICATION_ENABLE (command complete, error);
//          GET_CAPABILITY; GET_CONNECTOR_CAPABILITY and GET_CONNECTOR_STATUS of connector 1 (piano's one port);
//          SET_NOTIFICATION_ENABLE with every notification the capabilities allow; a last read of the CCI (a change that came early).
//   a command  the write (CONTROL), its answer, then the completion: a notification says "read the CCI", and the read that shows
//          COMMAND_COMPLETE (busy CCIs are ignored) carries MESSAGE_IN too; then ACK_CC_CI (command complete, and the connector
//          change when it was a change's status and no error) until ACK_COMPLETE. An error is followed by GET_ERROR_STATUS (logged).
//          If no notification comes, a read 250 ms after the write's answer and then every 500 ms finds the completion anyway (and
//          counts it: whether the DSP notifies is a fact to learn); 10 s without one is a timeout.
//   ready  nothing is sent and nothing polls: the next notification's CCI says which connector changed; its GET_CONNECTOR_STATUS is
//          a new status event (the kext decides the role from it). A change storm (more than 10 in 10 s) is not followed until a
//          notification after the window (no timer: a PPM that keeps reporting a change it cannot report does not keep us busy).
//   failure  a timeout, an error during the init or a refused read or write: the client fails (logged with the step) and starts
//          over after 10 s, at most three attempts per generation; then it stays failed until the next generation (nothing polls).
// One request on the wire at a time (the PPM answers in order); the relay's ready/generation restart it (stop, start).
#pragma once
#include "Ucsi.h"

namespace pianoucsi {

constexpr uint32_t kWireTimeoutMs = 5000;        // a read's or write's answer (ucsi_glink waits 5 s)
constexpr uint32_t kCommandTimeoutMs = 10000;    // a command's completion, the reset's (UCSI_TIMEOUT_MS)
constexpr uint32_t kResetPollMs = 20;            // ucsi_reset_ppm's steps
constexpr uint32_t kCompletionFirstPollMs = 250, kCompletionPollMs = 500;
constexpr uint32_t kRetryMs = 10000;
constexpr unsigned kAttempts = 3;
constexpr unsigned kConnector = 1;
constexpr uint32_t kStormWindowMs = 10000;
constexpr unsigned kStormChanges = 10;
constexpr uint32_t kSendRetryMs = 20;
constexpr unsigned kBusyRetries = 50;

enum class Phase : uint8_t { off, init, ready, failed };
enum class Step : uint8_t {
  none, version, clearReset, reset, enableBasic, capability, connectorCapability, status, enableAll, finalRead, change, errorStatus, user
};
inline const char *stepName(Step s)
{
  switch (s) {
    case Step::version: return "the first read (VERSION, CCI)";
    case Step::clearReset: return "clearing a latched reset completion";
    case Step::reset: return "PPM_RESET";
    case Step::enableBasic: return "SET_NOTIFICATION_ENABLE (command complete, error)";
    case Step::capability: return "GET_CAPABILITY";
    case Step::connectorCapability: return "GET_CONNECTOR_CAPABILITY";
    case Step::status: return "GET_CONNECTOR_STATUS";
    case Step::enableAll: return "SET_NOTIFICATION_ENABLE (all supported)";
    case Step::finalRead: return "the last read of the init";
    case Step::change: return "a connector change (GET_CONNECTOR_STATUS)";
    case Step::errorStatus: return "GET_ERROR_STATUS";
    case Step::user: return "a requested command";
    default: return "-";
  }
}

enum Event : uint32_t {
  kEvReady = 1u << 0,      // the init went through: capability, connector capability and the first status are valid
  kEvStatus = 1u << 1,     // a new connector status (the init's first, or a change's)
  kEvFailed = 1u << 2,     // a step failed (failure, failedStep); a retry follows unless kEvGaveUp
  kEvGaveUp = 1u << 3,     // no attempt left in this generation
  kEvUserDone = 1u << 4,   // the requested command finished (userResult)
  kEvBogus = 1u << 5,      // a change was reported for a connector that does not exist
  kEvNotify = 1u << 6,     // a notification arrived
  kEvStorm = 1u << 7,      // changes are not followed until the storm window ends
  kEvError = 1u << 8,      // a command ended with ERROR or NOT_SUPPORTED (lastError has GET_ERROR_STATUS's word)
};

struct Request {
  uint32_t length = 0;
  uint8_t data[kMaxRequest] = {};
};

class Client {
 public:
  // ---- results (read by the kext between calls) ----
  Phase phase = Phase::off;
  uint32_t generation = 0;
  uint16_t version = 0;
  uint32_t buffer = kBufferV1;       // the mailbox size the writes use (by the version)
  uint32_t answerBytes = 0;          // the mailbox size the first read answer carried
  uint32_t cci = 0;                  // the last CCI read
  Capability cap;
  ConnectorCapability concap;
  ConnectorStatus status;
  uint64_t enabled = 0;              // the notifications enabled last
  const char *failure = "";
  Step failedStep = Step::none;
  uint32_t failedCci = 0;
  uint16_t lastError = 0;            // GET_ERROR_STATUS after the last error
  uint8_t lastErrorCommand = 0;
  unsigned attempts = 0;
  uint8_t userCommand = 0;
  int userResult = 0;                // 0 ok, 1 error (lastError), 2 not supported, -1 timeout or refused
  uint32_t notify0 = 0;              // the first word of the last notification (for the log; Linux reads the CCI instead)
  struct Counters {
    uint32_t reads, writes, notifies, commands, byNotify, byPoll, busy, errors, notSupported, timeouts, changes, bogus, resends, stale,
        refusedReads, sendRetries, storms;
  } count = {};

  // ---- the relay's ready flag ----
  void start(uint32_t gen, uint64_t now)
  {
    stop();
    generation = gen;
    attempts = 0;
    beginInit(now);
  }
  void stop()
  {
    const Counters keep = count;
    const uint32_t gen = generation;
    *this = Client();
    count = keep;
    generation = gen;
  }

  // ---- a message of owner 0x800b ----
  void receive(const uint8_t *d, uint32_t length, uint64_t now)
  {
    if (phase == Phase::off || !d || length < kHeader || get32(d) != kOwner) return;
    const uint32_t opcode = get32(d + 8) & 0xffu;
    if (opcode == kOpNotify) {
      count.notifies++;
      events |= kEvNotify;
      notify0 = length >= 16 ? get32(d + 12) : 0;
      if (phase == Phase::failed) return;
      if (storming && now >= stormEnd) storming = false;
      notifyPending = true;
    } else if (opcode == kOpRead) {
      if (wire != Wire::read || outboxFull) {
        count.stale++;
        return;
      }
      wire = Wire::idle;
      const uint32_t bytes = length - kHeader >= 4 ? length - kHeader - 4 : 0;
      if (bytes < kMbMessageIn + 16) {
        count.refusedReads++;
        failNow(now, "a read answer too short for the mailbox");
        return;
      }
      const uint32_t ret = get32(d + kHeader + bytes);
      if (ret) {
        count.refusedReads++;
        failNow(now, "the PPM refused a read (non-zero return code)");
        return;
      }
      onRead(d + kHeader, bytes, now);
    } else if (opcode == kOpWrite) {
      if (wire != Wire::write || outboxFull) {
        count.stale++;
        return;
      }
      wire = Wire::idle;
      if (length >= kHeader + 4 && get32(d + kHeader)) {
        failNow(now, "the PPM refused a write (non-zero return code)");
        return;
      }
      onWritten(now);
    }
    pump(now);
  }

  // ---- time ----
  void tick(uint64_t now)
  {
    if (phase == Phase::failed) {
      if (retryAt && now >= retryAt) {
        retryAt = 0;
        beginInit(now);
      }
      return;
    }
    if (phase == Phase::off) return;
    if (wire != Wire::idle && !outboxFull && now >= wireDue) {
      count.timeouts++;
      failNow(now, wire == Wire::read ? "no answer to a read in 5 s" : "no answer to a write in 5 s");
      return;
    }
    if (kind != Kind::none && jobDue && now >= jobDue) {
      if (kind == Kind::asyncClear) {   // Linux tries the reset anyway
        endJob(now);
      } else {
        count.timeouts++;
        failNow(now, kind == Kind::asyncReset ? "no reset completion in 10 s" : "no command completion in 10 s");
        return;
      }
    }
    pump(now);
  }

  // The request to send now (stays until sent(true)); false when none.
  bool take(Request &out, uint64_t now) const
  {
    if (!outboxFull || now < holdUntil) return false;
    out.length = outbox.length;
    for (uint32_t i = 0; i < outbox.length; ++i) out.data[i] = outbox.data[i];
    return true;
  }
  // The relay took it (ok) or refused it (full, not ready: tried again in 20 ms).
  void sent(bool ok, uint64_t now)
  {
    if (!outboxFull) return;
    if (!ok) {
      count.sendRetries++;
      holdUntil = now + kSendRetryMs;
      return;
    }
    outboxFull = false;
    wireDue = now + kWireTimeoutMs;
  }

  // The next time tick() must run; 0: nothing is scheduled (ready and idle, or given up: nothing polls).
  uint64_t deadline() const
  {
    uint64_t d = 0;
    const auto earliest = [&d](uint64_t t) { if (t && (!d || t < d)) d = t; };
    if (phase == Phase::failed) {
      earliest(retryAt);
      return d;
    }
    if (phase == Phase::off) return 0;
    if (outboxFull) earliest(holdUntil ? holdUntil : 1);
    else if (wire != Wire::idle) earliest(wireDue);
    if (wire == Wire::idle && !outboxFull) earliest(readAt);
    if (kind != Kind::none) earliest(jobDue);
    if (phase == Phase::ready && kind == Kind::none && wire == Wire::idle && !outboxFull && (userQueued || (changePending && !storming)))
      earliest(1);   // work to start now
    return d;
  }

  uint32_t takeEvents()
  {
    const uint32_t e = events;
    events = 0;
    return e;
  }

  // SET_UOR / SET_PDR for connector 1 (a test switch; nothing else is accepted), run once ready and idle. False when refused.
  bool request(uint64_t ctl)
  {
    const uint8_t c = commandOf(ctl);
    if ((c != kSetUor && c != kSetPdr) || ((ctl >> 16) & 0x7fu) != kConnector || userQueued) return false;
    userControl = ctl;
    userQueued = true;
    return true;
  }

  bool busy() const { return kind != Kind::none || wire != Wire::idle || outboxFull; }
  Step currentStep() const { return step; }

 private:
  enum class Wire : uint8_t { idle, read, write };
  enum class Kind : uint8_t { none, read, asyncClear, asyncReset, sync, ack };
  Wire wire = Wire::idle;
  uint64_t wireDue = 0, holdUntil = 0;
  Request outbox;
  bool outboxFull = false;
  bool notifyPending = false;      // a notification to follow with a read
  bool readByNotify = false;       // the read on the wire was a notification's
  uint64_t readAt = 0;             // a poll read is due then
  Kind kind = Kind::none;
  Step step = Step::none, resumeStep = Step::none;
  uint64_t control = 0, jobDue = 0;
  bool connAck = false, awaiting = false;
  bool resultError = false, resultNotSupported = false;
  uint8_t messageIn[256] = {};
  uint32_t messageBytes = 0;
  bool changesOn = false;          // the final notification enable went through: connector changes are followed
  unsigned changePending = 0, changeHandled = 0;
  bool storming = false;
  uint64_t stormStart = 0, stormEnd = 0;
  unsigned stormCount = 0;
  uint64_t userControl = 0;
  bool userQueued = false;
  uint64_t retryAt = 0;
  uint32_t events = 0;
  unsigned busyRetries = 0;

  // -------------------------------------------------------------------------------------------------------- the wire
  void sendRead(uint64_t now, bool byNotify)
  {
    outbox.length = readRequest(outbox.data);
    outboxFull = true;
    holdUntil = 0;
    wire = Wire::read;
    wireDue = now + kWireTimeoutMs;
    readByNotify = byNotify;
    readAt = 0;
    count.reads++;
  }
  void sendWrite(uint64_t now, uint64_t ctl)
  {
    outbox.length = writeRequest(outbox.data, buffer, ctl);
    outboxFull = true;
    holdUntil = 0;
    wire = Wire::write;
    wireDue = now + kWireTimeoutMs;
    count.writes++;
  }
  // What to send next while the wire is free: a notification's read, a poll read when due, or the next job / change / request.
  void pump(uint64_t now)
  {
    if (phase != Phase::init && phase != Phase::ready) return;
    if (wire != Wire::idle || outboxFull) return;
    if (notifyPending) {
      notifyPending = false;
      sendRead(now, true);
      return;
    }
    if (readAt && now >= readAt) {
      sendRead(now, false);
      return;
    }
    if (phase == Phase::ready && kind == Kind::none) {
      if (changePending && !storming) {
        const unsigned n = changePending;
        changePending = 0;
        if (!stormStart || now - stormStart >= kStormWindowMs) {
          stormStart = now;
          stormCount = 0;
        }
        if (++stormCount > kStormChanges) {
          storming = true;
          stormEnd = stormStart + kStormWindowMs;
          count.storms++;
          events |= kEvStorm;
          return;
        }
        changeHandled = n;
        step = Step::change;
        beginSync(now, kGetConnectorStatus | connectorNumber(n), true);
        return;
      }
      if (userQueued) {
        userQueued = false;
        userCommand = commandOf(userControl);
        step = Step::user;
        beginSync(now, userControl, false);
      }
    }
  }

  // --------------------------------------------------------------------------------------------------------- the jobs
  void beginRead(uint64_t now)
  {
    kind = Kind::read;
    jobDue = 0;
    resultError = resultNotSupported = false;
    sendRead(now, false);
  }
  void beginAsync(uint64_t now, Kind k, uint64_t ctl)
  {
    kind = k;
    control = ctl;
    awaiting = false;
    resultError = resultNotSupported = false;
    jobDue = now + kCommandTimeoutMs;
    sendWrite(now, ctl);
  }
  void beginSync(uint64_t now, uint64_t ctl, bool ack)
  {
    kind = Kind::sync;
    control = ctl;
    connAck = ack;
    awaiting = false;
    resultError = resultNotSupported = false;
    messageBytes = 0;
    jobDue = now + kCommandTimeoutMs;
    count.commands++;
    sendWrite(now, ctl);
  }
  void onWritten(uint64_t now)
  {
    switch (kind) {
      case Kind::asyncClear:
      case Kind::asyncReset:
        awaiting = true;
        readAt = now + kResetPollMs;
        break;
      case Kind::sync:
      case Kind::ack:
        awaiting = true;
        readAt = now + kCompletionFirstPollMs;
        break;
      default:
        break;
    }
  }
  void onRead(const uint8_t *mb, uint32_t bytes, uint64_t now)
  {
    const bool byNotify = readByNotify;
    cci = get32(mb + kMbCci);
    if (cci & kCciBusy) count.busy++;
    // A connector change (ucsi_notify_common): followed once the final notification enable is through, the one being handled not
    // again (the PPM keeps reporting it until the acknowledgement).
    if (!(cci & kCciBusy) && cciConnector(cci) && changesOn) {
      const unsigned n = cciConnector(cci);
      if (cap.connectors && n > cap.connectors) {
        count.bogus++;
        events |= kEvBogus;
      } else if (!(kind == Kind::sync && step == Step::change && n == changeHandled) &&
                 !(kind == Kind::ack && step == Step::change && n == changeHandled)) {
        changePending = n;
      }
    }
    switch (kind) {
      case Kind::read:
        if (step == Step::version) {
          version = get16(mb + kMbVersion);
          answerBytes = bytes;
          buffer = bufferFor(version);
        }
        endJob(now);
        return;
      case Kind::asyncClear:
        if (cci & kCciCommandComplete) {
          endJob(now);
          return;
        }
        readAt = now + kResetPollMs;
        return;
      case Kind::asyncReset:
        if (cci & kCciResetComplete) {
          endJob(now);
          return;
        }
        if (cci & ~kCciResetComplete) {   // the PPM is still doing something else: reset it again
          count.resends++;
          sendWrite(now, control);
          return;
        }
        readAt = now + kResetPollMs;
        return;
      case Kind::sync:
        if (awaiting && !(cci & kCciBusy) && (cci & kCciCommandComplete)) {
          if (byNotify) count.byNotify++;
          else count.byPoll++;
          resultNotSupported = (cci & kCciNotSupported) != 0;
          resultError = !resultNotSupported && (cci & kCciError) != 0;
          if (resultNotSupported) count.notSupported++;
          if (resultError) count.errors++;
          const uint32_t room = bytes > kMbMessageIn ? bytes - kMbMessageIn : 0, want = messageInBytes(version);
          messageBytes = room < want ? room : want;
          for (uint32_t i = 0; i < messageBytes; ++i) messageIn[i] = mb[kMbMessageIn + i];
          // The acknowledgement: the command, and the connector change when this was a change's status and it went well.
          kind = Kind::ack;
          awaiting = false;
          jobDue = now + kCommandTimeoutMs;
          sendWrite(now, kAckCcCi | kAckCommandComplete | (connAck && !resultError && !resultNotSupported ? kAckConnectorChange : 0));
          return;
        }
        if (awaiting) readAt = now + kCompletionPollMs;
        return;
      case Kind::ack:
        if (awaiting && !(cci & kCciBusy) && (cci & kCciAckComplete)) {
          endJob(now);
          return;
        }
        if (awaiting) readAt = now + kCompletionPollMs;
        return;
      default:
        // Ready and idle: a notification's read (the change, if any, is pending now). A busy PPM's CCI says nothing: read again in
        // 20 ms, at most 50 times in a row (a second), then wait for the next notification.
        if ((cci & kCciBusy) && busyRetries < kBusyRetries) {
          busyRetries++;
          readAt = now + kResetPollMs;
        } else if (!(cci & kCciBusy)) {
          busyRetries = 0;
        }
        return;
    }
  }

  void endJob(uint64_t now)
  {
    kind = Kind::none;
    jobDue = 0;
    readAt = 0;
    awaiting = false;
    advance(now);
  }

  // -------------------------------------------------------------------------------------------------------- the steps
  void beginInit(uint64_t now)
  {
    if (attempts >= kAttempts) {
      phase = Phase::failed;
      events |= kEvGaveUp;
      return;
    }
    attempts++;
    phase = Phase::init;
    changesOn = false;
    changePending = 0;
    cap = Capability();
    concap = ConnectorCapability();
    status = ConnectorStatus();
    enabled = 0;
    wire = Wire::idle;
    outboxFull = false;
    notifyPending = false;
    readAt = 0;
    step = Step::version;
    beginRead(now);
  }

  // The step that just ended (its job done) decides the next.
  void advance(uint64_t now)
  {
    const bool bad = resultError || resultNotSupported;
    // An error is followed by GET_ERROR_STATUS (Linux's ucsi_read_error; the connector number only from UCSI 2.0 on), then the step
    // that had it decides what to do.
    if (step != Step::errorStatus && step != Step::version && step != Step::clearReset && step != Step::reset && step != Step::finalRead &&
        resultError) {
      resumeStep = step;
      lastErrorCommand = commandOf(control);
      step = Step::errorStatus;
      beginSync(now, kGetErrorStatus, false);
      return;
    }
    if (step == Step::errorStatus) {
      lastError = !bad && messageBytes >= 2 ? get16(messageIn) : 0;
      events |= kEvError;
      step = resumeStep;
      resultError = true;   // the resumed step had the error
      resultNotSupported = false;
    } else if (resultNotSupported) {
      lastErrorCommand = commandOf(control);
      lastError = 0;
      events |= kEvError;
    }
    const bool failed = resultError || resultNotSupported;
    switch (step) {
      case Step::version:
        if (!version) {
          failNow(now, "the PPM reports no version");
          return;
        }
        if (cci & kCciResetComplete) {
          step = Step::clearReset;
          beginAsync(now, Kind::asyncClear, kSetNotificationEnable);
        } else {
          step = Step::reset;
          beginAsync(now, Kind::asyncReset, kPpmReset);
        }
        return;
      case Step::clearReset:
        step = Step::reset;
        beginAsync(now, Kind::asyncReset, kPpmReset);
        return;
      case Step::reset:
        step = Step::enableBasic;
        enabled = kNtfyCommandComplete | kNtfyError;
        beginSync(now, kSetNotificationEnable | enabled, false);
        return;
      case Step::enableBasic:
        if (failed) return failStep(now, "SET_NOTIFICATION_ENABLE was refused");
        step = Step::capability;
        beginSync(now, kGetCapability, false);
        return;
      case Step::capability:
        if (failed) return failStep(now, "GET_CAPABILITY was refused");
        if (messageBytes < 16) return failStep(now, "GET_CAPABILITY: MESSAGE_IN too short");
        cap = decodeCapability(messageIn);
        if (!cap.connectors) return failStep(now, "the PPM has no connector");
        step = Step::connectorCapability;
        beginSync(now, kGetConnectorCapability | connectorNumber(kConnector), false);
        return;
      case Step::connectorCapability:
        if (failed) return failStep(now, "GET_CONNECTOR_CAPABILITY was refused");
        concap = decodeConnectorCapability(messageIn);
        step = Step::status;
        beginSync(now, kGetConnectorStatus | connectorNumber(kConnector), false);
        return;
      case Step::status:
        if (failed) return failStep(now, "GET_CONNECTOR_STATUS was refused");
        status = decodeStatus(messageIn, messageBytes, version);
        events |= kEvStatus;
        step = Step::enableAll;
        enabled = supportedNotifications(version, cap.features);
        beginSync(now, kSetNotificationEnable | enabled, false);
        return;
      case Step::enableAll:
        if (failed) return failStep(now, "SET_NOTIFICATION_ENABLE (all) was refused");
        changesOn = true;
        step = Step::finalRead;
        beginRead(now);
        return;
      case Step::finalRead:
        step = Step::none;
        phase = Phase::ready;
        events |= kEvReady;
        pump(now);
        return;
      case Step::change:
        changeHandled = 0;
        step = Step::none;
        if (failed) {
          failure = "GET_CONNECTOR_STATUS after a change was refused";
          failedStep = Step::change;
          failedCci = cci;
          events |= kEvFailed;
        } else {
          count.changes++;
          status = decodeStatus(messageIn, messageBytes, version);
          events |= kEvStatus;
        }
        pump(now);
        return;
      case Step::user:
        step = Step::none;
        userResult = resultError ? 1 : resultNotSupported ? 2 : 0;
        events |= kEvUserDone;
        pump(now);
        return;
      default:
        step = Step::none;
        return;
    }
  }

  void failStep(uint64_t now, const char *why) { failNow(now, why); }

  // A failure: logged by the kext from failure/failedStep; in the ready state a refused change status is not one (advance() logs it).
  void failNow(uint64_t now, const char *why)
  {
    failure = why;
    failedStep = step;
    failedCci = cci;
    events |= kEvFailed;
    if (step == Step::user) {
      userResult = -1;
      events |= kEvUserDone;
    }
    kind = Kind::none;
    wire = Wire::idle;
    outboxFull = false;
    notifyPending = false;
    readAt = jobDue = 0;
    step = Step::none;
    changesOn = false;
    phase = Phase::failed;
    if (attempts < kAttempts) {
      retryAt = now + kRetryMs;
    } else {
      retryAt = 0;
      events |= kEvGaveUp;
    }
  }
};

}  // namespace pianoucsi
