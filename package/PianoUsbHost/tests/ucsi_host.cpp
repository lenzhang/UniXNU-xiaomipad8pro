// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of PianoUsbHost's UCSI client (UcsiClient.h), its codec (Ucsi.h), the port's witnesses and the role switch (PortRole.h)
// against a simulated PPM: the DSP's UCSI owner 0x800b as the framing of package/include/PianoUsbcContract.h carries it (a read is
// the header alone and is answered with the 48-byte (1.x) or 528-byte (2.x) mailbox and a return code; a write carries a whole
// mailbox image and is answered with a return code; a notification says "read the CCI"). The model follows the UCSI specification's
// PPM: a reset clears the notification enable and latches RESET_COMPLETE, every command completes in CCI (with the MESSAGE_IN length),
// ACK_CC_CI clears the command completion and, with the connector-change bit, the connector's pending change; a change is shown in the
// CCI's connector field until it is acknowledged; notifications go out only for enabled events.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. ucsi_host.cpp -o /tmp/ucsi_host && /tmp/ucsi_host
#include "UcsiClient.h"
#include "PortRole.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using namespace pianoucsi;
using pianousbhost::PortView;
using pianousbhost::PortWitnesses;
using pianousbhost::Role;
using pianousbhost::RoleSwitch;
using pianousbhost::SwitchAction;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ----------------------------------------------------------------------------------------------------------- the PPM model
struct Ppm {
  uint16_t version = kV1_1;     // tonight's PPM answered version 1.10 with a 48-byte buffer
  uint32_t buffer = kBufferV1;
  uint32_t cci = 0;
  uint8_t messageIn[256] = {};
  uint64_t lastControl = 0;
  uint64_t ntfy = 0;            // the notifications enabled
  bool resetLatched = false;    // RESET_COMPLETE latched from before (a PPM reset nobody acknowledged)
  // behaviour switches
  bool notifyCommands = true;   // notify command/ack completions when enabled (the DSP may not: the client polls then)
  unsigned busyReads = 0;       // the next N reads show BUSY
  unsigned resetReads = 0;      // RESET_COMPLETE only after N reads following PPM_RESET
  bool dropWrites = false;      // answer no write (a stuck PPM)
  bool refuseWrites = false;    // answer writes with return code 1
  bool uorRejected = false;     // SET_UOR ends with ERROR, GET_ERROR_STATUS: swap rejected
  bool uorUnsupported = false;
  // the connector
  struct Con {
    bool connected = false, provider = false;
    uint8_t partner = 0, flags = 0, mode = 0;
    uint16_t change = 0;        // bits accumulated since the last acknowledgement
    bool pending = false;       // a change to report in the CCI
    int orientation = 0;        // 2.x only
  } con;
  uint16_t error = 0;
  unsigned connectorsReported = 1;
  // traffic
  std::deque<std::vector<uint8_t>> out;   // to the client
  std::vector<uint8_t> commands;          // every command written, in order
  std::vector<uint64_t> controls;
  unsigned reads = 0, writes = 0, resetsSeen = 0, notifies = 0;
  bool inReset = false;
  unsigned readsSinceReset = 0;

  void notify()
  {
    std::vector<uint8_t> m(kHeader + 12, 0);
    put32(&m[0], kOwner);
    put32(&m[4], kTypeRequest);
    put32(&m[8], kOpNotify);
    put32(&m[12], cci);
    out.push_back(m);
    notifies++;
  }
  // The CCI's connector field: a pending change, shown while no command is being reported over it.
  void showChange()
  {
    cci &= ~0xfeu;
    if (con.pending) cci |= 1u << 1;
  }
  void handleRead()
  {
    reads++;
    if (inReset) {
      readsSinceReset++;
      if (readsSinceReset > resetReads) {
        inReset = false;
        cci = kCciResetComplete;
      }
    }
    std::vector<uint8_t> m(kHeader + buffer + 4, 0);
    put32(&m[0], kOwner);
    put32(&m[4], kTypeRequest);
    put32(&m[8], kOpRead);
    m[kHeader + 0] = uint8_t(version);
    m[kHeader + 1] = uint8_t(version >> 8);
    uint32_t c = cci;
    if (busyReads) {
      busyReads--;
      c = kCciBusy | 0x00ffff00u;   // the rest is bogus while busy
    }
    put32(&m[kHeader + kMbCci], c);
    put32(&m[kHeader + kMbControl], uint32_t(lastControl));
    put32(&m[kHeader + kMbControl + 4], uint32_t(lastControl >> 32));
    const uint32_t in = messageInBytes(version);
    for (uint32_t i = 0; i < in && kMbMessageIn + i < buffer; ++i) m[kHeader + kMbMessageIn + i] = messageIn[i];
    out.push_back(m);
  }
  void complete(uint32_t bits, uint32_t length)
  {
    cci = bits | length << 8;
    showChange();
    if (notifyCommands && (ntfy & kNtfyCommandComplete)) notify();
  }
  void statusBytes()
  {
    std::memset(messageIn, 0, sizeof messageIn);
    uint32_t w = con.change | uint32_t(con.mode & 7u) << 16 | uint32_t(con.connected) << 19 | uint32_t(con.provider) << 20 |
                 uint32_t(con.flags) << 21 | uint32_t(con.partner & 7u) << 29;
    put32(messageIn, w);
    put32(messageIn + 4, 0x12345678u);   // an RDO
    messageIn[8] = 1;                    // nominal charging
    if (version >= kV2_0 && con.orientation) messageIn[10] |= 1u << 6;   // bit 86
  }
  void handleWrite(const uint8_t *d, uint32_t length)
  {
    writes++;
    if (dropWrites) return;
    std::vector<uint8_t> a(kHeader + 4, 0);
    put32(&a[0], kOwner);
    put32(&a[4], kTypeRequest);
    put32(&a[8], kOpWrite);
    if (length != kHeader + buffer + 4) {
      put32(&a[12], 2);   // the wrong image size
      out.push_back(a);
      return;
    }
    if (refuseWrites) {
      put32(&a[12], 1);
      out.push_back(a);
      return;
    }
    out.push_back(a);
    const uint64_t control = uint64_t(get32(d + kHeader + kMbControl)) | uint64_t(get32(d + kHeader + kMbControl + 4)) << 32;
    lastControl = control;
    const uint8_t command = commandOf(control);
    commands.push_back(command);
    controls.push_back(control);
    switch (command) {
      case kPpmReset:
        resetsSeen++;
        ntfy = 0;
        cci = 0;
        inReset = true;
        readsSinceReset = 0;
        resetLatched = false;
        return;   // no notification: the enable is gone
      case kSetNotificationEnable:
        if (resetLatched || (cci & kCciResetComplete)) resetLatched = false;
        ntfy = control & 0xffffffff0000ull;
        complete(kCciCommandComplete, 0);
        return;
      case kGetCapability:
        std::memset(messageIn, 0, sizeof messageIn);
        put32(messageIn, kAttrBatteryCharging | kAttrUsbPd);
        messageIn[4] = uint8_t(connectorsReported);
        messageIn[5] = uint8_t(kFeatAltModeDetails);   // alternate mode details, nothing else
        messageIn[8] = 1;
        messageIn[10] = 0x20; messageIn[11] = 0x01;   // BC 1.2
        messageIn[12] = 0x00; messageIn[13] = 0x03;   // PD 3.0
        messageIn[14] = 0x00; messageIn[15] = 0x02;   // Type-C 2.0
        complete(kCciCommandComplete, 16);
        return;
      case kGetConnectorCapability:
        std::memset(messageIn, 0, sizeof messageIn);
        messageIn[0] = uint8_t(kOpDrp | kOpUsb2 | kOpUsb3 | kOpAltMode);
        messageIn[1] = uint8_t((kCapProvider | kCapConsumer | kCapSwapToDfp | kCapSwapToUfp) >> 8);
        complete(kCciCommandComplete, 2);
        return;
      case kGetConnectorStatus:
        if (((control >> 16) & 0x7fu) != 1) {
          error = 1u << 1;
          complete(kCciCommandComplete | kCciError, 0);
          return;
        }
        statusBytes();
        complete(kCciCommandComplete, version >= kV2_0 ? 19 : 9);
        return;
      case kAckCcCi: {
        if (control & kAckConnectorChange) {
          con.change = 0;
          con.pending = false;
        }
        cci = kCciAckComplete;
        showChange();
        if (notifyCommands && (ntfy & kNtfyCommandComplete)) notify();
        return;
      }
      case kSetUor:
        if (uorUnsupported) {
          complete(kCciCommandComplete | kCciNotSupported, 0);
          return;
        }
        if (uorRejected) {
          error = 1u << 12;
          complete(kCciCommandComplete | kCciError, 0);
          return;
        }
        complete(kCciCommandComplete, 0);
        return;
      case kGetErrorStatus:
        std::memset(messageIn, 0, sizeof messageIn);
        messageIn[0] = uint8_t(error);
        messageIn[1] = uint8_t(error >> 8);
        complete(kCciCommandComplete, 2);
        return;
      default:
        complete(kCciCommandComplete | kCciNotSupported, 0);
        return;
    }
  }
  void receive(const uint8_t *d, uint32_t length)
  {
    if (length < kHeader || get32(d) != kOwner) return;
    const uint32_t op = get32(d + 8);
    if (op == kOpRead && length == kHeader) handleRead();
    else if (op == kOpWrite) handleWrite(d, length);
  }
  // The partner changes (plug, unplug, a data-role swap): the PPM shows the change and notifies if that is enabled.
  void event(uint16_t changeBits)
  {
    con.change |= changeBits;
    con.pending = true;
    if (!(cci & (kCciCommandComplete | kCciAckComplete))) cci = 0;
    showChange();
    const uint64_t wanted = (changeBits & kChangeConnect ? kNtfyConnect : 0) | (changeBits & kChangePartner ? kNtfyPartner : 0) |
                            (changeBits & kChangePowerDirection ? kNtfyPowerDirection : 0);
    if (ntfy & wanted) notify();
  }
  void plug(uint8_t partner, bool provider, uint8_t mode = kModeDefault, int orientation = 0)
  {
    con.connected = true;
    con.partner = partner;
    con.provider = provider;
    con.flags = kPartnerFlagUsb;
    con.mode = mode;
    con.orientation = orientation;
    event(kChangeConnect | kChangePowerDirection);
  }
  void unplug()
  {
    con.connected = false;
    con.partner = 0;
    con.flags = 0;
    con.mode = 0;
    event(kChangeConnect);
  }
};

// ----------------------------------------------------------------------------------------------------------- the harness
struct Rig {
  Ppm ppm;
  Client c;
  uint64_t now = 1000;
  uint32_t events = 0;
  bool deliverWrites = true;

  // Runs until nothing is due within ms (or ms has passed).
  void run(uint64_t ms)
  {
    const uint64_t end = now + ms;
    for (unsigned guard = 0; guard < 200000; ++guard) {
      Request r;
      if (c.take(r, now)) {
        c.sent(true, now);
        now += 1;
        ppm.receive(r.data, r.length);
        continue;
      }
      if (!ppm.out.empty()) {
        const std::vector<uint8_t> m = ppm.out.front();
        ppm.out.pop_front();
        now += 1;
        c.receive(m.data(), uint32_t(m.size()), now);
        events |= c.takeEvents();
        continue;
      }
      const uint64_t d = c.deadline();
      if (!d || d > end) {
        if (now < end) now = end;
        return;
      }
      if (d > now) now = d;
      c.tick(now);
      events |= c.takeEvents();
    }
    std::printf("FAIL: the rig did not settle\n");
    ++failures;
  }
  uint32_t take()
  {
    const uint32_t e = events | c.takeEvents();
    events = 0;
    return e;
  }
};

static std::vector<uint8_t> initSequence()
{
  return {kPpmReset, kSetNotificationEnable, kAckCcCi, kGetCapability, kAckCcCi, kGetConnectorCapability, kAckCcCi, kGetConnectorStatus,
          kAckCcCi, kSetNotificationEnable, kAckCcCi};
}

static void testCodec()
{
  // GET_CAPABILITY's layout.
  uint8_t m[16] = {0x06, 0, 0, 0, 0x81, 0x14, 0x00, 0, 2, 0, 0x20, 0x01, 0x00, 0x03, 0x00, 0x02};
  const Capability c = decodeCapability(m);
  CHECK(c.connectors == 1 && c.reservedBit && c.features == 0x14 && c.altModes == 2 && c.bcVersion == 0x120 && c.pdVersion == 0x300 &&
        c.typecVersion == 0x200 && (c.attributes & kAttrUsbPd));
  // The notifications: 1.x without PDO details or a PD reset notification, then 2.0's sink path.
  const uint64_t n = supportedNotifications(kV1_1, kFeatAltModeDetails);
  CHECK(n == (kNtfyAll & ~(kNtfyPowerLevel | kNtfyCapabilities | kNtfyExternalSupply | kNtfyPdReset)));
  CHECK((n & kNtfyConnect) && (n & kNtfyPartner) && (n & kNtfyCommandComplete) && (n & kNtfyCam));
  CHECK(!(supportedNotifications(kV1_1, 0) & kNtfyCam));
  CHECK(supportedNotifications(kV2_0, 0) & kNtfySinkPath);
  CHECK(!(supportedNotifications(kV1_2, 0) & kNtfySinkPath));
  // GET_CONNECTOR_STATUS: connected, provider, UFP, USB; orientation only from 2.0.
  uint8_t s[16] = {};
  put32(s, uint32_t(kChangeConnect) | 1u << 16 | 1u << 19 | 1u << 20 | 1u << 21 | 2u << 29);
  s[10] = 1u << 6;
  ConnectorStatus st = decodeStatus(s, 16, kV1_1);
  CHECK(st.connected && st.provider && st.partnerType == kPartnerUfp && (st.partnerFlags & kPartnerFlagUsb) && st.powerMode == kModeDefault &&
        (st.change & kChangeConnect) && st.orientation == -1);
  st = decodeStatus(s, 16, kV2_0);
  CHECK(st.orientation == 1);
  // CONTROL words.
  CHECK(setUor(1, true, false) == (0x09ull | 1ull << 16 | 1ull << 23));
  CHECK(setUor(1, false, true) == (0x09ull | 1ull << 16 | 1ull << 24 | 1ull << 25));
  CHECK(setPdr(1, true, false) == (0x0bull | 1ull << 16 | 1ull << 23));
  uint8_t w[kMaxRequest];
  CHECK(writeRequest(w, kBufferV1, kGetConnectorStatus | connectorNumber(1)) == 64);
  CHECK(get32(w) == 0x800b && get32(w + 4) == 1 && get32(w + 8) == 0x12 && get32(w + 12 + 8) == 0x10012 && get32(w + 12 + 12) == 0);
  CHECK(writeRequest(w, kBufferV2, kPpmReset) == 544);
  CHECK(readRequest(w) == 12 && get32(w + 8) == 0x11);
  CHECK(cciConnector(1u << 1 | kCciCommandComplete) == 1 && cciLength(9u << 8) == 9);
  // The port notification: reverse, USB3 only, the DP SVID in the opcode's high half.
  uint8_t p[32] = {};
  put32(p, 0x800c);
  put32(p + 4, 1);
  put32(p + 8, 0x16 | 0xff01u << 16);
  p[12] = 0;
  p[13] = 1;
  p[14] = kMuxUsb3;
  p[16] = 0xd1; p[17] = 0x2d;
  p[20] = 0x43;
  PortNotification pn = decodePort(p, 32);
  CHECK(pn.ok && pn.full && pn.port == 0 && pn.orientation == 1 && pn.mux == kMuxUsb3 && pn.svid == kSvidDp && pn.vid == 0x2dd1 &&
        pn.dp == 0x43);
  CHECK(portOrientation(0) == 0 && portOrientation(1) == 1 && portOrientation(2) == -1);
  CHECK(decodePort(p, 14).ok == false);
  put32(p + 8, 0x15);
  CHECK(!decodePort(p, 32).ok);
  CHECK(std::strcmp(errorName(1u << 12), "swap rejected") == 0);
}

// The init against tonight's PPM (1.10, 48 bytes), nothing on the port.
static void testInit()
{
  Rig r;
  r.c.start(1, r.now);
  r.run(20000);
  CHECK(r.c.phase == Phase::ready);
  CHECK(r.take() & kEvReady);
  CHECK(r.ppm.commands == initSequence());
  CHECK(r.c.version == kV1_1 && r.c.buffer == kBufferV1 && r.c.answerBytes == 48);
  CHECK(r.c.cap.valid && r.c.cap.connectors == 1 && r.c.cap.pdVersion == 0x300);
  CHECK(r.c.concap.valid && (r.c.concap.bits & kOpDrp) && (r.c.concap.bits & kCapSwapToDfp));
  CHECK(r.c.status.valid && !r.c.status.connected);
  CHECK(r.ppm.controls[1] == (kSetNotificationEnable | kNtfyCommandComplete | kNtfyError));
  CHECK(r.ppm.controls[9] == (kSetNotificationEnable | supportedNotifications(kV1_1, kFeatAltModeDetails)));
  CHECK(r.c.enabled == supportedNotifications(kV1_1, kFeatAltModeDetails));
  CHECK(r.ppm.ntfy == r.c.enabled);
  CHECK(r.ppm.controls[2] == (kAckCcCi | kAckCommandComplete));   // the init's status is acknowledged without the change bit
  CHECK(r.c.count.byNotify == 5 && r.c.count.byPoll == 0);   // the five commands' completions came with notifications
  // Ready and idle: nothing scheduled, nothing sent.
  CHECK(r.c.deadline() == 0);
  const unsigned reads = r.ppm.reads, writes = r.ppm.writes;
  r.run(600000);
  CHECK(r.ppm.reads == reads && r.ppm.writes == writes);
  // A latched reset completion is cleared first (SET_NOTIFICATION_ENABLE with no bit), then the reset.
  Rig l;
  l.ppm.cci = kCciResetComplete;
  l.c.start(1, l.now);
  l.run(20000);
  CHECK(l.c.phase == Phase::ready);
  CHECK(l.ppm.commands.size() == 12 && l.ppm.commands[0] == kSetNotificationEnable && l.ppm.controls[0] == kSetNotificationEnable &&
        l.ppm.commands[1] == kPpmReset);
  // A slow reset (RESET_COMPLETE after 5 reads, 20 ms apart).
  Rig s;
  s.ppm.resetReads = 5;
  s.c.start(1, s.now);
  s.run(20000);
  CHECK(s.c.phase == Phase::ready && s.ppm.resetsSeen == 1);
}

static PortView view(const Client &c, int gpio = -1, const PortNotification *pan = nullptr)
{
  PortWitnesses w;
  w.ucsiReady = c.phase == Phase::ready;
  w.ucsiVersion = c.version;
  w.ucsi = c.status;
  if (gpio >= 0) {
    w.gpioRead = true;
    w.gpioHigh = gpio != 0;
  }
  if (pan) {
    w.panSeen = true;
    w.pan = *pan;
  }
  return pianousbhost::portView(w);
}

static PortNotification panOf(uint8_t orientation, uint8_t mux)
{
  uint8_t p[32] = {};
  put32(p, 0x800c);
  put32(p + 4, 1);
  put32(p + 8, 0x16);
  p[13] = orientation;
  p[14] = mux;
  return decodePort(p, 32);
}

// Plugs: an OTG adapter (a UFP; we source VBUS), the Mac (a DFP; we sink), a dock (DFP first, a UFP after its data-role swap),
// the unplug. Each change: a notification, GET_CONNECTOR_STATUS, ACK_CC_CI with the change bit; the PPM's change cleared.
static void testChanges()
{
  Rig r;
  r.c.start(1, r.now);
  r.run(20000);
  r.take();
  const size_t base = r.ppm.commands.size();
  r.ppm.plug(kPartnerUfp, true);
  r.run(5000);
  CHECK(r.take() & kEvStatus);
  CHECK(r.ppm.commands.size() == base + 2 && r.ppm.commands[base] == kGetConnectorStatus && r.ppm.commands[base + 1] == kAckCcCi);
  CHECK(r.ppm.controls[base + 1] == (kAckCcCi | kAckCommandComplete | kAckConnectorChange));
  CHECK(!r.ppm.con.pending && r.ppm.con.change == 0);
  CHECK(r.c.status.connected && r.c.status.partnerType == kPartnerUfp && r.c.status.provider && (r.c.status.change & kChangeConnect));
  CHECK(r.c.count.changes == 1);
  PortNotification pan = panOf(1, kMuxUsb3);
  PortView v = view(r.c, 1, &pan);
  CHECK(v.role.role == Role::host && v.connected && v.vbus == 1);
  CHECK(v.orientation == 2 && v.agree && v.panO == 2 && v.gpioO == 2 && v.ucsiO == 0);   // UCSI 1.x has no orientation
  v = view(r.c, 0, &pan);
  CHECK(!v.agree);
  CHECK(r.c.deadline() == 0);
  // Unplug: device.
  r.ppm.unplug();
  r.run(5000);
  CHECK((r.take() & kEvStatus) && !r.c.status.connected);
  v = view(r.c, 1, &pan);
  CHECK(v.role.role == Role::device && !v.connected && v.vbus == -1 && !v.orientationKnown);   // gpio61 means nothing without a cable
  // The Mac.
  r.ppm.plug(kPartnerDfp, false, kModeTypec3_0);
  r.run(5000);
  v = view(r.c);
  CHECK(v.role.role == Role::device && v.vbus == 0 && r.c.status.powerMode == kModeTypec3_0);
  // A dock after its data-role swap: a partner change (DFP -> UFP), power still from the dock.
  r.ppm.con.partner = kPartnerUfp;
  r.ppm.event(kChangePartner);
  r.run(5000);
  v = view(r.c);
  CHECK(v.role.role == Role::host && v.vbus == 0 && (r.c.status.change & kChangePartner));
  CHECK(r.c.count.changes == 4);
  // A change for a connector that does not exist: counted, not followed.
  r.ppm.cci = 5u << 1;
  r.ppm.notify();
  const size_t before = r.ppm.commands.size();
  r.run(5000);
  CHECK((r.take() & kEvBogus) && r.ppm.commands.size() == before);
}

// No notification from the PPM at all: the completions are found by reads (250 ms, then every 500 ms), and with nothing to read
// the client stays idle (a plug is then not seen: the log's counters show it).
static void testNoNotifications()
{
  Rig r;
  r.ppm.notifyCommands = false;
  r.c.start(1, r.now);
  r.run(30000);
  CHECK(r.c.phase == Phase::ready);
  CHECK(r.c.count.byNotify == 0 && r.c.count.byPoll == 5);    // the five commands' completions (the acknowledgements' too: polled)
  CHECK(r.c.deadline() == 0);
  // BUSY reads are ignored (the CCI is bogus while busy).
  Rig b;
  b.c.start(1, b.now);
  b.run(20000);
  b.ppm.busyReads = 2;
  b.ppm.plug(kPartnerUfp, true);
  b.run(10000);
  CHECK(b.c.status.connected && b.c.count.busy >= 2 && b.c.phase == Phase::ready);
}

// A stuck PPM: no answer to a write -> a timeout after 5 s, a new attempt after 10 s; three attempts, then nothing more (nothing
// polls) until the next generation.
static void testFailures()
{
  Rig r;
  r.ppm.dropWrites = true;
  r.c.start(1, r.now);
  r.run(6000);
  CHECK(r.c.phase == Phase::failed && (r.take() & kEvFailed) && r.c.failedStep == Step::reset && r.c.count.timeouts == 1);
  CHECK(r.c.deadline() != 0);
  r.ppm.dropWrites = false;
  r.run(20000);
  CHECK(r.c.phase == Phase::ready && r.c.attempts == 2);
  Rig g;
  g.ppm.dropWrites = true;
  g.c.start(1, g.now);
  g.run(120000);
  CHECK(g.c.phase == Phase::failed && (g.take() & kEvGaveUp) && g.c.attempts == kAttempts && g.c.deadline() == 0);
  const unsigned writes = g.ppm.writes;
  g.run(600000);
  CHECK(g.ppm.writes == writes);
  // A new generation starts over.
  g.ppm.dropWrites = false;
  g.c.start(2, g.now);
  g.run(20000);
  CHECK(g.c.phase == Phase::ready && g.c.generation == 2 && g.c.attempts == 1);
  // A refused write (non-zero return code) fails at once.
  Rig f;
  f.ppm.refuseWrites = true;
  f.c.start(1, f.now);
  f.run(1000);
  CHECK(f.c.phase == Phase::failed && f.c.count.timeouts == 0);
  // A generation that ends mid-command: the old answers are stale.
  Rig m;
  m.c.start(1, m.now);
  Request q;
  CHECK(m.c.take(q, m.now));
  m.c.sent(true, m.now);
  m.ppm.receive(q.data, q.length);   // the first read's answer waits in m.ppm.out
  m.c.stop();
  m.c.start(2, m.now);
  m.run(20000);
  CHECK(m.c.phase == Phase::ready && m.c.count.stale >= 1);
}

// SET_UOR (a test switch): accepted, not supported, rejected (GET_ERROR_STATUS: swap rejected).
static void testUserCommands()
{
  Rig r;
  r.c.start(1, r.now);
  r.run(20000);
  r.take();
  CHECK(!r.c.request(kGetCapability));   // only SET_UOR / SET_PDR
  CHECK(!r.c.request(setUor(2, true, false)));
  CHECK(r.c.request(setUor(1, true, false)));
  r.run(5000);
  CHECK((r.take() & kEvUserDone) && r.c.userResult == 0 && r.c.userCommand == kSetUor);
  r.ppm.uorUnsupported = true;
  CHECK(r.c.request(setUor(1, true, false)));
  r.run(5000);
  CHECK(r.c.userResult == 2 && (r.take() & kEvError));
  r.ppm.uorUnsupported = false;
  r.ppm.uorRejected = true;
  CHECK(r.c.request(setUor(1, false, true)));
  r.run(5000);
  CHECK(r.c.userResult == 1 && r.c.lastError == (1u << 12) && r.c.lastErrorCommand == kSetUor);
  CHECK(r.c.phase == Phase::ready);
  CHECK(r.ppm.commands.back() == kAckCcCi);
  // A plug while a command runs: the change is seen in that command's CCI and followed once it is through.
  r.ppm.uorRejected = false;
  CHECK(r.c.request(setUor(1, true, false)));
  r.c.tick(r.now);
  Request q;
  CHECK(r.c.take(q, r.now));
  r.c.sent(true, r.now);
  r.ppm.receive(q.data, q.length);   // SET_UOR written; its answer and completion wait in the PPM's queue
  r.ppm.plug(kPartnerUfp, true);
  const uint32_t changes = r.c.count.changes;
  r.run(5000);
  CHECK(r.c.userResult == 0 && r.c.count.changes == changes + 1 && r.c.status.connected && !r.ppm.con.pending);
}

// UCSI 2.0 (528-byte mailbox): 544-byte writes, the orientation bit as the first witness.
static void testV2()
{
  Rig r;
  r.ppm.version = kV2_0;
  r.ppm.buffer = kBufferV2;
  r.c.start(1, r.now);
  r.run(20000);
  CHECK(r.c.phase == Phase::ready && r.c.buffer == kBufferV2 && r.c.answerBytes == kBufferV2);
  CHECK(r.c.enabled & kNtfySinkPath);
  r.ppm.plug(kPartnerUfp, true, kModeDefault, 1);
  r.run(5000);
  const PortView v = view(r.c, 1);
  CHECK(v.ucsiO == 2 && v.orientation == 2 && v.agree);
}

// A storm: a PPM that keeps reporting changes. More than 10 in 10 s are not followed; a notification after the window resumes.
static void testStorm()
{
  Rig r;
  r.c.start(1, r.now);
  r.run(20000);
  r.take();
  for (int i = 0; i < 15; ++i) {
    r.ppm.event(kChangePartner);
    r.run(50);
  }
  CHECK(r.c.count.changes == 10 && r.c.count.storms == 1 && (r.take() & kEvStorm));
  CHECK(r.c.deadline() == 0);
  r.run(11000);
  CHECK(r.c.count.changes == 10);
  r.ppm.event(kChangePartner);
  r.run(5000);
  CHECK(r.c.count.changes == 11);
}

// The role switch: off by default ("would switch"), the debounce, device at once, no second try after a failure, the deferral.
static void testRoleSwitch()
{
  RoleSwitch off;
  CHECK(off.step(Role::host, true, 0) == SwitchAction::none);
  CHECK(off.step(Role::host, true, 5000) == SwitchAction::none && off.wouldSwitch == 1 && off.deadline() == 0);
  RoleSwitch s;
  s.enabled = true;
  CHECK(s.step(Role::device, true, 0) == SwitchAction::none);
  CHECK(s.step(Role::host, true, 100) == SwitchAction::none);
  CHECK(s.deadline() == 1100);
  CHECK(s.step(Role::host, true, 1099) == SwitchAction::none);
  CHECK(s.step(Role::host, true, 1100) == SwitchAction::toHost);
  s.done(SwitchAction::toHost, true);
  CHECK(s.current == Role::host && s.deadline() == 0);
  CHECK(s.step(Role::host, true, 2000) == SwitchAction::none);
  CHECK(s.step(Role::device, false, 2001) == SwitchAction::toDevice);   // the unplug: at once
  s.done(SwitchAction::toDevice, true);
  CHECK(s.current == Role::device && s.toHost == 1 && s.toDevice == 1);
  CHECK(s.step(Role::none, true, 2100) == SwitchAction::none);   // an audio accessory: device mode stays armed
  // A dock: DFP first, then a UFP: the debounce runs from the swap.
  RoleSwitch d;
  d.enabled = true;
  d.step(Role::device, true, 0);
  d.step(Role::host, true, 300);
  CHECK(d.step(Role::device, true, 800) == SwitchAction::none);   // flapping back: the debounce restarts
  CHECK(d.step(Role::host, true, 900) == SwitchAction::none && d.deadline() == 1900);
  CHECK(d.step(Role::host, true, 1900) == SwitchAction::toHost);
  // A failed host attempt: not again until the partner goes away.
  d.done(SwitchAction::toHost, false);
  CHECK(d.current == Role::device && d.failedThisConnection && d.deadline() == 0);
  CHECK(d.step(Role::host, true, 10000) == SwitchAction::none);
  d.step(Role::device, false, 11000);
  CHECK(!d.failedThisConnection);
  d.step(Role::host, true, 12000);
  CHECK(d.step(Role::host, true, 13000) == SwitchAction::toHost);
  // Deferred (PianoDwc3 still starting): asked again at the deferral's end.
  d.defer(15000);
  CHECK(d.deadline() == 15000);
  CHECK(d.step(Role::host, true, 14000) == SwitchAction::none);
  CHECK(d.step(Role::host, true, 15000) == SwitchAction::toHost);
}

// The end to end path the kext runs: an OTG adapter plugged, the decision host after the debounce; unplugged, device at once.
static void testEndToEnd()
{
  Rig r;
  r.c.start(1, r.now);
  r.run(20000);
  RoleSwitch s;
  s.enabled = true;
  PortView v = view(r.c);
  CHECK(s.step(v.role.role, v.connected, r.now) == SwitchAction::none);
  r.ppm.plug(kPartnerUfp, true);
  r.run(100);
  v = view(r.c);
  CHECK(v.role.role == Role::host && s.step(v.role.role, v.connected, r.now) == SwitchAction::none);
  const uint64_t due = s.deadline();
  CHECK(due == r.now + 1000);
  r.now = due;
  CHECK(s.step(v.role.role, v.connected, r.now) == SwitchAction::toHost);
  s.done(SwitchAction::toHost, true);
  r.ppm.unplug();
  r.run(100);
  v = view(r.c);
  CHECK(s.step(v.role.role, v.connected, r.now) == SwitchAction::toDevice);
}

int main()
{
  testCodec();
  testInit();
  testChanges();
  testNoNotifications();
  testFailures();
  testUserCommands();
  testV2();
  testStorm();
  testRoleSwitch();
  testEndToEnd();
  if (failures) {
    std::printf("ucsi_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("ucsi_host: all UCSI client, witness and role switch tests passed\n");
  return 0;
}
