// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoWifi level 5: the QMI codec (WifiQmi.h), the copied IPC router with its two additions (WifiQrtr.c), the two
// MHI channels (WifiChannels.h), the WLAN firmware's QMI client (WifiWlfw.h) and the 32-bit window for the endpoint's stream
// (WifiSmmuWindow.h). What it holds the code to:
//   the codec: every scalar, string and counted-data field round-trips; a message that would not fit its buffer is refused whole,
//             and a half-open TLV is refused too;
//   the router: the chip's hello is answered, its service announcement is remembered with its instance word, a data packet built
//             in a transport buffer carries the right header, and a received packet reaches our port's callback;
//   the channels: the two contexts carry only their fixed fields before the start (disabled, burst mode disabled, the type, the
//             event ring; every other channel untouched), a start before the channel is prepared is refused by the chip model,
//             a start prepares the ring (base a multiple of its length, both pointers at the base) and enables the channel before
//             its command, 20 before 21 and one at a time; the start commands go on the command ring and their completions start
//             the channels, every inbound element but one gets a buffer, a packet is sent with one doorbell per submission, the
//             event walk delivers a received packet, frees the outbound buffer, and reports state and execution-environment
//             changes; a bogus event is counted, never acted on; the M0 answer rings every event ring and running channel;
//   MHI after mission mode: the device wake doorbell (channel 127, high half first, refused outside the register space), the
//             host's state write (M2), the health read (a system error, the crash dump environment, a device that is gone);
//   the crash dump (WifiRddm.h): the plan (segments aligned to their size, the table on its own page, every entry), the arm's
//             order (address high, low, size, doorbell with the session), the receive status (complete, an error, waiting, all
//             ones), and the decode against synthetic dumps: the header in both word orders, files back to back across segments,
//             the failure reason read across a segment boundary, bounded and sanitised, the serial masked, the fallback strings
//             (the strong ones first, format strings never, at most four of at most 120 characters), an all-zero dump, a header
//             with no names, a file larger than the dump;
//   the client: the whole exchange against a simulated firmware — the radio's capability, the indication registration, the host
//             capability, the memory request answered out of the window, the chip's capability, the regulatory database and the
//             board file in 6 KiB segments with the right total/segment/end fields and the right bytes, the two microcodes by
//             address, the trace configuration, the ready indication (level 5's gate), then with piano-wifi-mode=1 the options,
//             the configuration (nine pipes, fourteen services, the shadow list with piano-wifi-shadow=1) and the mode;
//   its faults: a refused request, a missing file, a region larger than the window (the "ask again smaller" form, and the smaller
//             request answered), a response with the wrong transaction, an indication in the wrong step, and a timeout;
//   the window: the plan's choice of group and bank, the table's descriptors and the translation of every block, the apply order
//             with its read-backs, and each refusal (no free bank, a masked group, a 32-bit output size).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. qmi_host.cpp ../WifiQrtr.c -o /tmp/qmi_host && /tmp/qmi_host
#include "WifiChannels.h"
#include "WifiQmi.h"
#include "WifiRddm.h"
#include "WifiSmmuWindow.h"
#include "WifiWlfw.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace pianowifi;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ============================================================================================================== the codec
static void testCodec()
{
  uint8_t buffer[256];
  QmiWriter w;
  qmiBegin(w, buffer, sizeof buffer, kQmiRequest, 0x1234, 0x0034);
  qmiU8(w, 0x10, 0x5a);
  qmiU16(w, 0x11, 0xbeef);
  qmiU32(w, 0x12, 0xdeadbeef);
  qmiU64(w, 0x13, 0x0123456789abcdefull);
  qmiString(w, 0x14, "WIN", 16);
  const uint8_t payload[5] = {1, 2, 3, 4, 5};
  qmiBytes(w, 0x15, payload, sizeof payload, 2);
  const uint32_t length = qmiEnd(w);
  CHECK(length == w.length && length > kQmiHeaderBytes);
  CHECK(buffer[0] == kQmiRequest && buffer[1] == 0x34 && buffer[2] == 0x12 && buffer[3] == 0x34 && buffer[4] == 0x00);
  CHECK((buffer[5] | buffer[6] << 8) == int(length - kQmiHeaderBytes));

  QmiMessage m;
  CHECK(qmiParse(buffer, length, m));
  CHECK(m.type == kQmiRequest && m.transaction == 0x1234 && m.message == 0x0034);
  uint8_t u8 = 0;
  uint16_t u16 = 0;
  uint32_t u32 = 0;
  uint64_t u64 = 0;
  CHECK(qmiReadU8(m, 0x10, &u8) && u8 == 0x5a);
  CHECK(qmiReadU16(m, 0x11, &u16) && u16 == 0xbeef);
  CHECK(qmiReadU32(m, 0x12, &u32) && u32 == 0xdeadbeef);
  CHECK(qmiReadU64(m, 0x13, &u64) && u64 == 0x0123456789abcdefull);
  uint32_t size = 0;
  const uint8_t *string = qmiFind(m, 0x14, &size);
  CHECK(string && size == 3 && std::memcmp(string, "WIN", 3) == 0);   // no count, no terminating zero
  const uint8_t *data = qmiFind(m, 0x15, &size);
  CHECK(data && size == 2 + sizeof payload && data[0] == sizeof payload && data[1] == 0 &&
        std::memcmp(data + 2, payload, sizeof payload) == 0);
  CHECK(!qmiReadU32(m, 0x10, &u32));     // the wrong size for that tag
  CHECK(!qmiReadU8(m, 0x7f, &u8));       // absent
  CHECK(!qmiScalar(m, 0x7f, 1, nullptr));

  // A structure read member by member, bounds-checked.
  QmiTlvReader r = qmiTlvReader(m, 0x15);
  CHECK(!r.bad && qmiTake(r, 2) == sizeof payload);
  for (uint8_t b : payload) CHECK(qmiTake(r, 1) == b);
  CHECK(!r.bad);
  (void)qmiTake(r, 1);
  CHECK(r.bad);                          // one byte past the TLV

  // The result TLV of a response.
  uint8_t response[32];
  QmiWriter rw;
  qmiBegin(rw, response, sizeof response, kQmiResponse, 7, 0x0024);
  qmiTlvBegin(rw, kQmiTlvResult);
  qmiLe(rw, 1, 2);
  qmiLe(rw, 0x53, 2);
  qmiTlvEnd(rw);
  const uint32_t responseLength = qmiEnd(rw);
  QmiMessage rm;
  CHECK(qmiParse(response, responseLength, rm));
  uint16_t result = 0, error = 0;
  CHECK(qmiResult(rm, &result, &error) && result == 1 && error == 0x53);

  // Overflow: the message is refused whole, and nothing was written past the buffer.
  uint8_t small[12];
  QmiWriter ow;
  qmiBegin(ow, small, sizeof small, kQmiRequest, 1, 2);
  qmiU64(ow, 0x10, 0);
  CHECK(ow.overflow && qmiEnd(ow) == 0);
  // A TLV left open is a programming error, not a message.
  QmiWriter hw;
  qmiBegin(hw, buffer, sizeof buffer, kQmiRequest, 1, 2);
  qmiTlvBegin(hw, 0x10);
  CHECK(qmiEnd(hw) == 0);
  // A buffer too small even for the header.
  QmiWriter tw;
  qmiBegin(tw, buffer, 4, kQmiRequest, 1, 2);
  CHECK(tw.overflow && qmiEnd(tw) == 0);
  // A message whose payload length lies about its size does not parse.
  uint8_t lying[16] = {kQmiResponse, 1, 0, 0x24, 0, 0xff, 0x00};
  QmiMessage lm;
  CHECK(!qmiParse(lying, sizeof lying, lm));
}

// ===================================================================================== the device, its memory and the chip
// The bus addresses the chip uses are the window's (an offset from kBusBase), so that the model can find any buffer the driver
// gives it; the real driver's are either physical (a bypassing stream) or the 32-bit window's (WifiSmmuWindow.h).
constexpr uint64_t kBusBase = 0x20000000;
constexpr uint32_t kWindowBytes = 8u << 20;

struct Device {
  std::vector<uint8_t> memory;          // the DMA window as the chip sees it
  std::map<uint32_t, uint32_t> regs;    // the MHI register block in BAR0
  std::map<uint32_t, unsigned> writesTo;   // how often each register was written (a doorbell rung twice with one value)
  std::vector<std::vector<uint8_t>> toHost;   // packets the firmware wants to send
  unsigned doorbells = 0, commandsSeen = 0, packetsTaken = 0, rxPosted = 0;
  uint32_t chdb = 0x300, erdb = 0x200, reglen = 0x1000;

  Device() : memory(kWindowBytes, 0)
  {
    regs[kMhiRegLen] = reglen;
    regs[kMhiChdbOff] = chdb;
    regs[kMhiErdbOff] = erdb;
    regs[kMhiBhiOff] = 0x500;
    regs[kMhiBhieOff] = 0x600;
    regs[kMhiCfg] = 0;
  }
  uint8_t *at(uint64_t bus, uint32_t bytes)
  {
    if (bus < kBusBase || bus + bytes > kBusBase + memory.size()) return nullptr;
    return memory.data() + (bus - kBusBase);
  }
  static uint32_t rd(void *ctx, uint32_t reg) { return static_cast<Device *>(ctx)->regs[reg]; }
  static void wr(void *ctx, uint32_t reg, uint32_t value)
  {
    Device &d = *static_cast<Device *>(ctx);
    d.regs[reg] = value;
    ++d.writesTo[reg];
    if (reg == d.chdb + 8 * kChanIpcrOut || reg == d.chdb + 8 * kChanIpcrIn || reg == kMhiCrdbLo) ++d.doorbells;
  }
  MhiIo io() { return MhiIo{this, &Device::rd, &Device::wr}; }

  uint64_t reg64(uint32_t low) { return uint64_t(regs[low]) | uint64_t(regs[low + 4]) << 32; }
};

static uint64_t get64(const uint8_t *at)
{
  uint64_t v = 0;
  for (unsigned i = 0; i < 8; ++i) v |= uint64_t(at[i]) << (8 * i);
  return v;
}
static uint32_t get32(const uint8_t *at)
{
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= uint32_t(at[i]) << (8 * i);
  return v;
}
static void put64(uint8_t *at, uint64_t v)
{
  for (unsigned i = 0; i < 8; ++i) at[i] = uint8_t(v >> (8 * i));
}
static void put32(uint8_t *at, uint32_t v)
{
  for (unsigned i = 0; i < 4; ++i) at[i] = uint8_t(v >> (8 * i));
}

// The chip's side of a channel: its read pointer walks the ring the host fills, up to the write pointer it was last told. Like the
// chip, the model learns that pointer from the context when the channel starts and from the channel's doorbell after that — a
// doorbell rung before the start is not seen (the context the start reads is what counts), and a ring the host fills without a
// doorbell is not read.
struct DeviceChannel {
  uint64_t rp = 0, wp = 0, lastDoorbell = 0;
  unsigned lastWrites = 0;   // how many times the doorbell's latching half had been written when the chip last looked
};

// What the chip does between two polls of the driver: consume the command ring, consume outbound packets, fill inbound buffers,
// and write the events for all of it into the event rings.
struct DeviceSide {
  Device &d;
  MhiContexts &contexts;
  DeviceChannel out, in, cmd;
  uint64_t evWp[kMhiEventRings] = {0, 0};   // where the chip writes next
  unsigned events = 0, staleDoorbells = 0;
  bool startedOut = false, startedIn = false;
  bool helloFirst = false;   // the chip's router says hello as soon as its inbound channel has a buffer (what the chip does)
  bool mergeEvents = false;  // one completion for every two inbound packets (a chip that merges them: only the second is named)
  bool dbModeOnce = false;   // the first outbound doorbell is answered with a doorbell-mode event, not taken

  DeviceSide(Device &device, MhiContexts &c) : d(device), contexts(c) {}

  uint8_t *contextOf(uint32_t chan) { return mhiChannelCtx(contexts, chan); }
  uint8_t *eventContext(unsigned ring) { return mhiEventCtx(contexts, ring); }
  void event(unsigned ring, uint64_t ptr, uint32_t word0, uint32_t word1)
  {
    uint8_t *ctx = eventContext(ring);
    const uint64_t base = get64(ctx + 12), length = get64(ctx + 20);
    if (!evWp[ring]) evWp[ring] = base;
    uint8_t *element = d.at(evWp[ring], kMhiElement);
    if (!element) return;
    put64(element, ptr);
    put32(element + 8, word0);
    put32(element + 12, word1);
    evWp[ring] += kMhiElement;
    if (evWp[ring] >= base + length) evWp[ring] = base;
    put64(ctx + 28, evWp[ring]);   // the chip's producer pointer, which the host reads
    ++events;
  }
  void run()
  {
    // the command ring (a start is refused, as the MHI specification has it, unless the host enabled the channel and gave it a
    // ring of whole elements on an event ring that exists, with its type set)
    uint8_t *cctx = mhiCommandCtx(contexts);
    const uint64_t cbase = get64(cctx + 12), clen = get64(cctx + 20), cwp = get64(cctx + 36);
    if (!cmd.rp) cmd.rp = cbase;
    while (cmd.rp != cwp) {
      const uint8_t *element = d.at(cmd.rp, kMhiElement);
      const uint32_t word1 = element ? get32(element + 12) : 0;
      const uint32_t chan = word1 >> 24, command = (word1 >> 16) & 0xffu;
      ++d.commandsSeen;
      const uint8_t *sctx = contextOf(chan);
      const bool startable = (get32(sctx) & 0xffu) == kChanStateEnabled && get32(sctx + kChanType) != 0 &&
                             get32(sctx + kChanErIndex) < kMhiEventRings && get64(sctx + kChanRLen) &&
                             get64(sctx + kChanRLen) % kMhiElement == 0 && get64(sctx + kChanRBase) % get64(sctx + kChanRLen) == 0;
      if (command == kTreCmdStart && startable) {
        uint8_t *ctx = contextOf(chan);
        put32(ctx, (get32(ctx) & ~0xffu) | kChanStateRunning);
        DeviceChannel *c = chan == kChanIpcrOut ? &out : chan == kChanIpcrIn ? &in : nullptr;
        if (c) {   // the start reads the ring from the context; a doorbell rung before it is stale
          c->rp = get64(ctx + kChanRp);
          c->wp = get64(ctx + kChanWp);
          c->lastDoorbell = d.reg64(d.chdb + 8 * chan);
          c->lastWrites = d.writesTo[d.chdb + 8 * chan];
        }
        if (chan == kChanIpcrOut) startedOut = true;
        if (chan == kChanIpcrIn) startedIn = true;
        event(0, cmd.rp, kEvCodeSuccess << 24, kEvTypeCmdCompletion << 16);
      } else {
        event(0, cmd.rp, 0x10u << 24, kEvTypeCmdCompletion << 16);
      }
      cmd.rp += kMhiElement;
      if (cmd.rp >= cbase + clen) cmd.rp = cbase;
    }
    // the doorbells since the last turn (a doorbell of a channel that is not running is counted and ignored)
    for (uint32_t chan : {kChanIpcrOut, kChanIpcrIn}) {
      DeviceChannel &c = chan == kChanIpcrOut ? out : in;
      const bool running = chan == kChanIpcrOut ? startedOut : startedIn;
      const uint64_t db = d.reg64(d.chdb + 8 * chan);
      const unsigned writes = d.writesTo[d.chdb + 8 * chan];
      if (writes == c.lastWrites) continue;
      c.lastWrites = writes;
      c.lastDoorbell = db;
      if (running) c.wp = db;
      else ++staleDoorbells;
    }
    // the outbound channel: every element the host added is a packet for the firmware; the completion names the element
    uint8_t *octx = contextOf(kChanIpcrOut);
    const uint64_t obase = get64(octx + 12), olen = get64(octx + 20);
    if (startedOut && dbModeOnce && out.rp != out.wp) {
      dbModeOnce = false;   // nothing taken: the host must ring again
      event(kIpcrEventRing, out.rp, kEvCodeDbMode << 24, kChanIpcrOut << 24 | kEvTypeTransfer << 16);
      out.wp = out.rp;
    }
    while (startedOut && out.rp != out.wp && out.wp >= obase && out.wp < obase + olen) {
      const uint8_t *element = d.at(out.rp, kMhiElement);
      const uint64_t ptr = element ? get64(element) : 0;
      const uint32_t length = element ? get32(element + 8) & 0xffffu : 0;
      const uint8_t *payload = d.at(ptr, length);
      if (payload) {
        ++d.packetsTaken;
        onPacket(std::vector<uint8_t>(payload, payload + length));
      }
      event(kIpcrEventRing, out.rp, kEvCodeEot << 24 | length, kChanIpcrOut << 24 | kEvTypeTransfer << 16);
      out.rp += kMhiElement;
      if (out.rp >= obase + olen) out.rp = obase;
      put64(octx + 28, out.rp);
    }
    // the inbound channel: each queued packet takes the next buffer the host posted
    uint8_t *ictx = contextOf(kChanIpcrIn);
    const uint64_t ibase = get64(ictx + 12), ilen = get64(ictx + 20);
    if (startedIn && helloFirst && !helloQueued && in.rp != in.wp && helloHook) {
      helloQueued = true;
      helloHook(hookContext);
    }
    unsigned merged = 0;
    while (startedIn && !d.toHost.empty() && in.rp != in.wp && in.wp >= ibase && in.wp < ibase + ilen) {
      const uint8_t *element = d.at(in.rp, kMhiElement);
      const uint64_t ptr = element ? get64(element) : 0;
      const uint32_t room = element ? get32(element + 8) & 0xffffu : 0;
      std::vector<uint8_t> packet = d.toHost.front();
      d.toHost.erase(d.toHost.begin());
      uint8_t *buffer = d.at(ptr, room);
      if (buffer && packet.size() <= room) {
        std::memcpy(buffer, packet.data(), packet.size());
        ++d.rxPosted;
        if (!mergeEvents || ++merged % 2 == 0)
          event(kIpcrEventRing, in.rp, kEvCodeEot << 24 | uint32_t(packet.size()), kChanIpcrIn << 24 | kEvTypeTransfer << 16);
      } else {
        event(kIpcrEventRing, in.rp, kEvCodeOverflow << 24 | room, kChanIpcrIn << 24 | kEvTypeTransfer << 16);
      }
      in.rp += kMhiElement;
      if (in.rp >= ibase + ilen) in.rp = ibase;
      put64(ictx + 28, in.rp);
    }
  }
  bool helloQueued = false;
  void (*helloHook)(void *) = nullptr;
  // Set by the firmware model.
  void (*packetHook)(void *, const std::vector<uint8_t> &) = nullptr;
  void *hookContext = nullptr;
  void onPacket(const std::vector<uint8_t> &packet)
  {
    if (packetHook) packetHook(hookContext, packet);
  }
};

// =================================================================================== the simulated firmware (QRTR and QMI)
constexpr uint32_t kFirmwareNode = 7, kFirmwarePort = 0x19;
// The board identifiers the simulated firmware gives (its capability answers are built by capture-less builders): the radio's
// capability's, and the chip capability's board information (absent when gSimCapBoard is false).
static uint32_t gSimPhyBoard = 0x21, gSimBoard = 0x21;
static bool gSimCapBoard = true;

struct Firmware {
  DeviceSide *device = nullptr;
  bool helloSent = false, announced = false;
  // what it checks and reports
  unsigned requests = 0, downloads = 0, boardSegments = 0, regdbSegments = 0, qdssSegments = 0;
  std::string board, regdb, qdss;
  uint64_t m3Address = 0, auxAddress = 0;
  uint32_t m3Bytes = 0, auxBytes = 0;
  std::vector<uint64_t> regionAddresses;
  std::vector<uint32_t> regionSizes;
  unsigned memRequests = 0, respondEmpty = 0;
  std::vector<uint32_t> shadow;
  unsigned pipes = 0, services = 0;
  uint32_t mode = 0xffffffffu;
  bool iniSeen = false, cfgSeen = false, ready = false, memReadySent = false;
  bool refuseBoard = false, refuseHostCap = false, answerPhyCap = true, refuseQdss = false;
  uint32_t lastFileId = 0;
  bool wrongTransaction = false, earlyMemReady = false, earlyMemRequest = false, earlyFwReady = false;
  uint32_t bigRegion = 0;              // a first memory request with one region this big
  uint32_t hostNode = 0, hostPort = 0;
  // Its router's flow control, the protocol's sending side as net/qrtr has it (and as the WLAN firmware showed on the tablet: ten
  // data packets to our port, then nothing): it counts the data packets sent to our port since the last RESUME_TX, asks for one on
  // the fifth (confirm_rx), and holds every packet from the tenth on in its own queue until a RESUME_TX names our node and port
  // (the destination it resumes). A RESUME_TX that names anything else resumes nothing (the copied router's old form: the sender).
  static constexpr unsigned kFlowLow = 5, kFlowHigh = 10;
  bool flowControl = true;
  unsigned pendingTx = 0, dataSent = 0, confirmsAsked = 0, resumes = 0, misaddressedResumes = 0;
  uint32_t resumeFromNode = 0, resumeFromPort = 0, resumeToNode = 0, resumeToPort = 0;
  std::vector<std::vector<uint8_t>> held;

  void queue(std::vector<uint8_t> packet) { device->d.toHost.push_back(std::move(packet)); }
  void sendData(std::vector<uint8_t> packet)
  {
    if (flowControl && pendingTx >= kFlowHigh) {
      held.push_back(std::move(packet));
      return;
    }
    ++pendingTx;
    ++dataSent;
    if (flowControl && pendingTx == kFlowLow) {
      put32(packet.data() + 16, 1);   // confirm_rx: answer with a RESUME_TX
      ++confirmsAsked;
    }
    queue(std::move(packet));
  }
  // A RESUME_TX from the host: its header, and its payload's client address.
  void onResume(const std::vector<uint8_t> &packet)
  {
    const uint32_t size = get32(packet.data() + 20);
    resumeFromNode = get32(packet.data() + 8);
    resumeFromPort = get32(packet.data() + 12);
    resumeToNode = get32(packet.data() + 24);
    resumeToPort = get32(packet.data() + 28);
    if (size < 12 || get32(packet.data() + QCOM_QRTR_HEADER) != QCOM_QRTR_RESUME_TX ||
        get32(packet.data() + QCOM_QRTR_HEADER + 4) != hostNode || get32(packet.data() + QCOM_QRTR_HEADER + 8) != hostPort) {
      ++misaddressedResumes;
      return;
    }
    ++resumes;
    pendingTx = 0;
    std::vector<std::vector<uint8_t>> waiting;
    waiting.swap(held);
    for (auto &p : waiting) sendData(std::move(p));
  }

  void control(uint32_t command, const uint32_t words[4])
  {
    std::vector<uint8_t> packet(QCOM_QRTR_HEADER + 20, 0);
    put32(packet.data(), 1);
    put32(packet.data() + 4, command);
    put32(packet.data() + 8, kFirmwareNode);
    put32(packet.data() + 12, QCOM_QRTR_PORT_CTRL);
    put32(packet.data() + 20, 20);
    put32(packet.data() + 24, QCOM_QRTR_NODE_BROADCAST);
    put32(packet.data() + 28, QCOM_QRTR_PORT_CTRL);
    put32(packet.data() + QCOM_QRTR_HEADER, command);
    for (unsigned i = 0; i < 4; ++i) put32(packet.data() + QCOM_QRTR_HEADER + 4 + 4 * i, words[i]);
    queue(std::move(packet));
  }
  void hello()
  {
    const uint32_t words[4] = {0, 0, 0, 0};
    control(QCOM_QRTR_HELLO, words);
    helloSent = true;
  }
  void announce()
  {
    const uint32_t words[4] = {kWlfwService, kWlfwInstanceWord, kFirmwareNode, kFirmwarePort};
    control(QCOM_QRTR_NEW_SERVER, words);
    announced = true;
  }
  void reply(const uint8_t *message, uint32_t length)
  {
    std::vector<uint8_t> packet(QCOM_QRTR_HEADER + ((length + 3) & ~3u), 0);
    put32(packet.data(), 1);
    put32(packet.data() + 4, QCOM_QRTR_DATA);
    put32(packet.data() + 8, kFirmwareNode);
    put32(packet.data() + 12, kFirmwarePort);
    put32(packet.data() + 20, length);
    put32(packet.data() + 24, hostNode);
    put32(packet.data() + 28, hostPort);
    std::memcpy(packet.data() + QCOM_QRTR_HEADER, message, length);
    sendData(std::move(packet));
  }
  void respond(uint16_t transaction, uint16_t message, uint16_t result, uint16_t error,
               void (*extra)(QmiWriter &) = nullptr)
  {
    uint8_t buffer[1024];
    QmiWriter w;
    qmiBegin(w, buffer, sizeof buffer, kQmiResponse, wrongTransaction ? uint16_t(transaction + 1) : transaction, message);
    qmiTlvBegin(w, kQmiTlvResult);
    qmiLe(w, result, 2);
    qmiLe(w, error, 2);
    qmiTlvEnd(w);
    if (extra) extra(w);
    const uint32_t length = qmiEnd(w);
    CHECK(length != 0);
    reply(buffer, length);
  }
  void indicate(uint16_t message, void (*build)(QmiWriter &) = nullptr)
  {
    uint8_t buffer[2048];
    QmiWriter w;
    qmiBegin(w, buffer, sizeof buffer, kQmiIndication, 0, message);
    if (build) build(w);
    const uint32_t length = qmiEnd(w);
    CHECK(length != 0);
    reply(buffer, length);
  }
  void requestMemory(const uint32_t *sizes, const uint32_t *types, unsigned count)
  {
    uint8_t buffer[2048];
    QmiWriter w;
    qmiBegin(w, buffer, sizeof buffer, kQmiIndication, 0, kWlfwRequestMemInd);
    qmiTlvBegin(w, 0x01);
    qmiLe(w, count, 1);
    for (unsigned i = 0; i < count; ++i) {
      qmiLe(w, sizes[i], 4);
      qmiLe(w, types[i], 4);
      qmiLe(w, 1, 1);          // one configuration entry per region, as the firmware describes its own view
      qmiLe(w, 0, 8);
      qmiLe(w, sizes[i], 4);
      qmiLe(w, 0, 1);
    }
    qmiTlvEnd(w);
    const uint32_t length = qmiEnd(w);
    CHECK(length != 0);
    ++memRequests;
    reply(buffer, length);
  }
  void firstMemoryRequest()
  {
    if (bigRegion) {
      const uint32_t sizes[2] = {bigRegion, 64 * 1024};
      const uint32_t types[2] = {kMemHostDdr, kMemCaldb};
      requestMemory(sizes, types, 2);
      return;
    }
    const uint32_t sizes[4] = {1u << 20, 512u << 10, 64u << 10, 32u << 10};
    const uint32_t types[4] = {kMemHostDdr, kMemM3Dump, kMemCaldb, kMemBdf};
    requestMemory(sizes, types, 4);
  }

  // One QMI message from the host (the payload of a data packet).
  void onMessage(const uint8_t *data, uint32_t length)
  {
    QmiMessage m;
    CHECK(qmiParse(data, length, m));
    CHECK(m.type == kQmiRequest);
    ++requests;
    switch (m.message) {
    case kWlfwPhyCap:
      CHECK(m.length == 0);
      if (!answerPhyCap) {
        respond(m.transaction, m.message, 1, 0x53);
        return;
      }
      respond(m.transaction, m.message, kQmiResultSuccess, 0, [](QmiWriter &w) {
        qmiU8(w, 0x10, 2);          // two radios
        qmiU32(w, 0x11, gSimPhyBoard);   // the board identifier (only logged by the client)
        qmiU8(w, 0x13, 1);          // single-chip multi-link
      });
      return;
    case kWlfwIndRegister: {
      uint8_t ready = 0, mem = 0, memReady = 0, init = 0, cal = 0;
      uint32_t client = 0;
      CHECK(qmiReadU8(m, 0x10, &ready) && ready == 1);
      CHECK(qmiReadU32(m, 0x15, &client) && client == kWlfwClientId);
      CHECK(qmiReadU8(m, 0x16, &mem) && mem == 1);
      CHECK(qmiReadU8(m, 0x17, &memReady) && memReady == 1);
      CHECK(qmiReadU8(m, 0x18, &init) && init == 1);
      CHECK(qmiReadU8(m, 0x1b, &cal) && cal == 1);
      CHECK(!qmiReadU8(m, 0x14, nullptr));   // the pin-connect result is not asked for
      respond(m.transaction, m.message, kQmiResultSuccess, 0, [](QmiWriter &w) { qmiU64(w, 0x10, 0x1234); });
      return;
    }
    case kWlfwHostCap: {
      uint32_t clients = 0;
      uint8_t nm = 0, bdf = 0, m3 = 0, m3cache = 0, calDone = 1, memMode = 0xff;
      uint64_t features = 0;
      CHECK(qmiReadU32(m, 0x10, &clients) && clients == 1);
      CHECK(qmiReadU8(m, 0x13, &nm) && nm == kHostCapNmModem);
      CHECK(qmiReadU8(m, 0x14, &bdf) && bdf == 1);
      CHECK(qmiReadU8(m, 0x16, &m3) && m3 == 1);
      CHECK(qmiReadU8(m, 0x17, &m3cache) && m3cache == 1);
      CHECK(qmiReadU8(m, 0x1a, &calDone) && calDone == 0);
      CHECK(qmiReadU8(m, 0x1c, &memMode) && memMode == kWlfwMemCfgModeDefault);
      CHECK(qmiReadU64(m, 0x27, &features) && features == kWlfwFeatureList);
      if (refuseHostCap) {
        respond(m.transaction, m.message, 1, 0x10);
        return;
      }
      if (earlyMemRequest) {   // the firmware asks for memory before it answers: the client must keep the request
        firstMemoryRequest();
        respond(m.transaction, m.message, kQmiResultSuccess, 0);
        return;
      }
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      firstMemoryRequest();
      return;
    }
    case kWlfwRespondMem: {
      QmiTlvReader r = qmiTlvReader(m, 0x01);
      CHECK(!r.bad);
      const uint32_t count = uint32_t(qmiTake(r, 1));
      if (!count) {
        ++respondEmpty;
        respond(m.transaction, m.message, 1, 0);   // the firmware refuses the empty form and asks again, smaller
        const uint32_t sizes[2] = {kWlfwChunkLimit, 64 * 1024};
        const uint32_t types[2] = {kMemHostDdr, kMemCaldb};
        bigRegion = 0;
        requestMemory(sizes, types, 2);
        return;
      }
      regionAddresses.clear();
      regionSizes.clear();
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t address = qmiTake(r, 8);
        const uint32_t size = uint32_t(qmiTake(r, 4));
        const uint32_t type = uint32_t(qmiTake(r, 4));
        const uint32_t restore = uint32_t(qmiTake(r, 1));
        CHECK(restore == 0);
        CHECK(!r.bad);
        if (wlfwMemWanted(type)) {
          CHECK(address >= kBusBase && address + size <= kBusBase + kWindowBytes);
          CHECK((address & 63) == 0);
        } else {
          CHECK(address == 0);
        }
        regionAddresses.push_back(address);
        regionSizes.push_back(size);
      }
      if (earlyMemReady) {   // "the memory is ready" before the answer it follows
        indicate(kWlfwFwMemReadyInd);
        respond(m.transaction, m.message, kQmiResultSuccess, 0);
        memReadySent = true;
        return;
      }
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      indicate(kWlfwFwMemReadyInd);
      memReadySent = true;
      return;
    }
    case kWlfwCap:
      CHECK(m.length == 0);
      respond(m.transaction, m.message, kQmiResultSuccess, 0, [](QmiWriter &w) {
        qmiTlvBegin(w, 0x10);          // chip
        qmiLe(w, 0x20, 4);
        qmiLe(w, 0x4b, 4);
        qmiTlvEnd(w);
        if (gSimCapBoard) {
          qmiTlvBegin(w, 0x11);        // board
          qmiLe(w, gSimBoard, 4);
          qmiTlvEnd(w);
        }
        qmiTlvBegin(w, 0x12);          // the system on chip
        qmiLe(w, 0x400, 4);
        qmiTlvEnd(w);
        qmiTlvBegin(w, 0x13);          // the firmware's version and its build time, a counted string inside the structure
        qmiLe(w, 0x301, 4);
        const char *when = "2026-01-01 00:00";
        qmiLe(w, std::strlen(when), 1);
        for (const char *p = when; *p; ++p) qmiLe(w, uint8_t(*p), 1);
        qmiTlvEnd(w);
        qmiString(w, 0x14, "WLAN.HMT.1.0", 128);
        qmiU8(w, 0x15, 1);
        qmiU32(w, 0x16, 950);
        qmiU32(w, 0x18, 0x11);
        qmiU64(w, 0x1a, 0x81);
      });
      return;
    case kWlfwBdfDownload: {
      uint8_t valid = 0, end = 0, type = 0;
      uint32_t id = 0, total = 0, segment = 0, size = 0;
      CHECK(qmiReadU8(m, 0x01, &valid) && valid == 1);
      CHECK(qmiReadU32(m, 0x10, &id) && id == (gSimCapBoard ? gSimBoard : kBoardIdDefault));   // the chip capability's board
      lastFileId = id;
      CHECK(qmiReadU32(m, 0x11, &total));
      CHECK(qmiReadU32(m, 0x12, &segment));
      const uint8_t *data = qmiFind(m, 0x13, &size);
      CHECK(data && size >= 2);
      const uint32_t bytes = uint32_t(data[0] | data[1] << 8);
      CHECK(size == bytes + 2 && bytes <= kWlfwMaxDataBytes);
      CHECK(qmiReadU8(m, 0x14, &end));
      CHECK(qmiReadU8(m, 0x15, &type));
      CHECK(total >= bytes);
      CHECK(end == (total == bytes ? 1 : 0));
      std::string &sink = type == kBdfTypeRegdb ? regdb : board;
      CHECK(segment == (type == kBdfTypeRegdb ? regdbSegments : boardSegments));
      sink.append(reinterpret_cast<const char *>(data + 2), bytes);
      if (type == kBdfTypeRegdb) ++regdbSegments;
      else ++boardSegments;
      ++downloads;
      if (refuseBoard && type == kBdfTypeElf) {
        respond(m.transaction, m.message, 1, 0x53);
        return;
      }
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      return;
    }
    case kWlfwQdssDownload: {
      uint8_t end = 0;
      uint32_t total = 0, segment = 0, size = 0;
      CHECK(qmiReadU32(m, 0x10, &total));
      CHECK(qmiReadU32(m, 0x11, &segment) && segment == qdssSegments);
      const uint8_t *data = qmiFind(m, 0x12, &size);
      CHECK(data && size >= 2);
      const uint32_t bytes = uint32_t(data[0] | data[1] << 8);
      CHECK(size == bytes + 2);
      CHECK(qmiReadU8(m, 0x13, &end) && end == (total == bytes ? 1 : 0));
      qdss.append(reinterpret_cast<const char *>(data + 2), bytes);
      ++qdssSegments;
      if (refuseQdss) {   // a firmware that does not take the trace configuration, and starts all the same
        respond(m.transaction, m.message, 1, 0x2f);
        indicate(kWlfwFwReadyInd);
        return;
      }
      if (end && earlyFwReady) {   // "ready" before the answer to the last segment
        indicate(kWlfwFwReadyInd);
        respond(m.transaction, m.message, kQmiResultSuccess, 0);
        return;
      }
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      if (end) indicate(kWlfwFwReadyInd);
      return;
    }
    case kWlfwM3Info:
      CHECK(qmiReadU64(m, 0x01, &m3Address) && qmiReadU32(m, 0x02, &m3Bytes));
      CHECK(m3Address >= kBusBase && m3Address + m3Bytes <= kBusBase + kWindowBytes);
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      return;
    case kWlfwAuxUcInfo:
      CHECK(qmiReadU64(m, 0x01, &auxAddress) && qmiReadU32(m, 0x02, &auxBytes));
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      return;
    case kWlfwWlanIni: {
      uint8_t log = 0;
      CHECK(qmiReadU8(m, 0x10, &log) && log == 1);
      iniSeen = true;
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      return;
    }
    case kWlfwWlanCfg: {
      uint32_t size = 0;
      const uint8_t *version = qmiFind(m, 0x10, &size);
      CHECK(version && size == 3 && std::memcmp(version, kHostVersionString, 3) == 0);
      QmiTlvReader pipeReader = qmiTlvReader(m, 0x11);
      CHECK(!pipeReader.bad);
      pipes = unsigned(qmiTake(pipeReader, 1));
      CHECK(pipes == kCePipeCount);
      for (unsigned i = 0; i < pipes; ++i) {
        CHECK(qmiTake(pipeReader, 4) == kCePipes[i].pipe);
        CHECK(qmiTake(pipeReader, 4) == kCePipes[i].dir);
        CHECK(qmiTake(pipeReader, 4) == kCePipes[i].entries);
        CHECK(qmiTake(pipeReader, 4) == kCePipes[i].bytesMax);
        CHECK(qmiTake(pipeReader, 4) == kCePipes[i].flags);
      }
      CHECK(!pipeReader.bad && pipeReader.used == pipeReader.size);
      QmiTlvReader serviceReader = qmiTlvReader(m, 0x12);
      CHECK(!serviceReader.bad);
      services = unsigned(qmiTake(serviceReader, 1));
      CHECK(services == kCeServiceCount);
      for (unsigned i = 0; i < services; ++i) {
        CHECK(qmiTake(serviceReader, 4) == kCeServices[i].service);
        CHECK(qmiTake(serviceReader, 4) == kCeServices[i].dir);
        CHECK(qmiTake(serviceReader, 4) == kCeServices[i].pipe);
      }
      CHECK(!serviceReader.bad && serviceReader.used == serviceReader.size);
      shadow.clear();
      QmiTlvReader shadowReader = qmiTlvReader(m, 0x17);
      if (!shadowReader.bad) {
        const unsigned count = unsigned(qmiTake(shadowReader, 1));
        for (unsigned i = 0; i < count; ++i) shadow.push_back(uint32_t(qmiTake(shadowReader, 4)));
        CHECK(!shadowReader.bad && shadowReader.used == shadowReader.size);
      }
      cfgSeen = true;
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      return;
    }
    case kWlfwWlanMode: {
      uint8_t debug = 1;
      CHECK(qmiReadU32(m, 0x01, &mode) && mode == kFirmwareModeNormal);
      CHECK(qmiReadU8(m, 0x10, &debug) && debug == 0);
      respond(m.transaction, m.message, kQmiResultSuccess, 0);
      ready = true;
      return;
    }
    default:
      CHECK(false);   // a message this firmware does not know
      return;
    }
  }
};

// ============================================================== the driver's side: the arena, the transport and the client
struct Host {
  Device device;
  Arena arena;
  MhiContexts contexts;
  Ipcr transport;
  QcomQrtr router;
  WlfwClient client;
  Firmware firmware;
  std::unique_ptr<DeviceSide> owned;
  DeviceSide *side = nullptr;
  std::map<std::string, std::string> files;
  unsigned steps = 0, lastStep = 0, sendRefusals = 0, fileReads = 0, refuseResumes = 0, resumeRefusals = 0;
  bool fileFails = false;
  std::vector<WlfwEvent> events;   // what the client told its owner (WlfwIo::event)

  uint8_t *memoryAt(uint64_t bus, uint32_t bytes) { return device.at(bus, bytes); }

  // ---- the QRTR link's send: the packet is already in one of our transport buffers, or is a small control packet to copy ----
  static int send(void *ctx, const uint8_t *packet, uint32_t length)
  {
    Host &h = *static_cast<Host *>(ctx);
    if (h.refuseResumes && length >= 8 && get32(packet + 4) == QCOM_QRTR_RESUME_TX) {   // a link that loses the answer
      ++h.resumeRefusals;
      return h.refuseResumes == 1 ? (h.refuseResumes = 0, -1) : -1;                     // 1: once, 2: always
    }
    for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
      if (h.transport.tx[i].va == packet) return h.transport.started[0] && ipcrSubmitTx(h.transport, h.device.io(), i, length)
                                                      ? 0 : (++h.sendRefusals, -1);
    const int index = ipcrClaimTx(h.transport);
    if (index < 0 || length > h.transport.tx[index].bytes) {
      ++h.sendRefusals;
      return -1;
    }
    std::memcpy(h.transport.tx[index].va, packet, length);
    return ipcrSubmitTx(h.transport, h.device.io(), unsigned(index), length) ? 0 : (++h.sendRefusals, -1);
  }
  // ---- what the client needs ----
  static uint8_t *claim(void *ctx, uint32_t *capacity)
  {
    Host &h = *static_cast<Host *>(ctx);
    const int index = ipcrClaimTx(h.transport);
    if (index < 0) return nullptr;
    *capacity = h.transport.tx[index].bytes - QCOM_QRTR_HEADER;
    return h.transport.tx[index].va + QCOM_QRTR_HEADER;   // the router's header goes in front of the message
  }
  static int sendMessage(void *ctx, uint8_t *buffer, uint32_t length)
  {
    Host &h = *static_cast<Host *>(ctx);
    uint8_t *base = buffer - QCOM_QRTR_HEADER;
    uint32_t node = 0, port = 0;
    if (qcom_qrtr_find_service(&h.router, kWlfwService, kWlfwInstanceWord, &node, &port)) return -1;
    for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
      if (h.transport.tx[i].va == base)
        return qcom_qrtr_send_buffer(&h.router, base, h.transport.tx[i].bytes, kQrtrClientPort, node, port, length);
    return -1;
  }
  static uint32_t fileSize(void *ctx, const char *name)
  {
    Host &h = *static_cast<Host *>(ctx);
    auto it = h.files.find(name);
    return it == h.files.end() ? 0 : uint32_t(it->second.size());
  }
  static int fileRead(void *ctx, const char *name, uint64_t offset, void *to, uint32_t bytes)
  {
    Host &h = *static_cast<Host *>(ctx);
    ++h.fileReads;
    if (h.fileFails) return -1;
    auto it = h.files.find(name);
    if (it == h.files.end() || offset + bytes > it->second.size()) return -1;
    std::memcpy(to, it->second.data() + offset, bytes);
    return 0;
  }
  static uint64_t memory(void *ctx, uint32_t bytes, uint8_t **va)
  {
    Host &h = *static_cast<Host *>(ctx);
    uint64_t bus = 0;
    uint8_t *p = arenaAlloc(h.arena, bytes, &bus);
    if (!p) return 0;
    if (va) *va = p;
    return bus;
  }
  static void step(void *ctx, int step, const char *)
  {
    Host &h = *static_cast<Host *>(ctx);
    ++h.steps;
    h.lastStep = unsigned(step);
  }
  static void event(void *ctx, const WlfwEvent &e)
  {
    static_cast<Host *>(ctx)->events.push_back(e);
  }
  // ---- a data packet for our port: the client's ----
  static void receive(void *ctx, QcomQrtr *, uint32_t, uint32_t, uint32_t, const uint8_t *data, uint32_t length)
  {
    Host &h = *static_cast<Host *>(ctx);
    wlfwReceive(h.client, data, length);
  }
  static void firmwarePacket(void *ctx, const std::vector<uint8_t> &packet)
  {
    Host &h = *static_cast<Host *>(ctx);
    // The firmware's router: a hello is answered with a hello and the service announcement; a data packet for its service port is
    // a QMI message.
    const uint32_t type = get32(packet.data() + 4), size = get32(packet.data() + 20), port = get32(packet.data() + 28);
    if (type == QCOM_QRTR_DATA) {
      h.firmware.hostNode = get32(packet.data() + 8);
      h.firmware.hostPort = get32(packet.data() + 12);
    }
    if (type == QCOM_QRTR_HELLO) {
      if (!h.firmware.helloSent) h.firmware.hello();
      if (!h.firmware.announced) h.firmware.announce();
      return;
    }
    if (type == QCOM_QRTR_RESUME_TX) {
      h.firmware.onResume(packet);
      return;
    }
    if (type != QCOM_QRTR_DATA || port != kFirmwarePort) return;
    h.firmware.onMessage(packet.data() + QCOM_QRTR_HEADER, size);
  }

  static void firmwareHello(void *ctx)
  {
    Host &h = *static_cast<Host *>(ctx);
    if (!h.firmware.helloSent) h.firmware.hello();
  }

  void start()
  {
    arenaInit(arena, device.memory.data(), kBusBase, uint32_t(device.memory.size()));
    uint64_t contextBus = 0;
    uint8_t *contextVa = arenaAlloc(arena, kMhiContextBytes, &contextBus);
    CHECK(contextVa != nullptr);
    CHECK(mhiContextsPlan(contexts, contextVa, contextBus, kMhiContextBytes));
    mhiContextsProgram(contexts, device.io(), device.reglen);
    CHECK(ipcrPlan(transport, arena, contexts));
    CHECK(ipcrOpen(transport, device.io(), device.reglen));
    owned.reset(new DeviceSide(device, contexts));
    side = owned.get();
    side->packetHook = &Host::firmwarePacket;
    side->helloHook = &Host::firmwareHello;
    side->hookContext = this;
    firmware.device = side;
    qcom_qrtr_init(&router, kQrtrLocalNode, &Host::send, this);
    CHECK(qcom_qrtr_add_client(&router, kQrtrClientPort, &Host::receive, this) == 0);
    files["regdb_xiaomi.bin"] = std::string(13000, 'r');      // three segments
    files["bd_p81.elf"] = std::string(6144, 'b');             // exactly one segment
    files["phy_ucode20.elf"] = std::string(2048, 'm');
    files["aux_ucode20.elf"] = std::string(1024, 'a');
    files["qdss_trace_config_v2.cfg"] = std::string(100, 'q');
  }

  // One turn of the machine: the chip runs, then the driver walks its event rings.
  unsigned turn()
  {
    side->run();
    const MhiIo io = device.io();
    unsigned packets = 0;
    ipcrPoll(transport, contexts, io, [&](uint32_t kind, uint32_t, uint32_t, uint32_t length, uint8_t *va) {
      if (kind != kIpcrPacket) return;
      ++packets;
      qcom_qrtr_receive(&router, va, length);
    });
    ipcrPostRx(transport, io);   // as the driver's pump: after every walk (the transport itself refuses before 21 runs)
    qcom_qrtr_poll(&router);     // and a flow-control answer the link refused, again
    return packets;
  }
  // The link up: the two channels started, the inbound buffers posted, the hello said and the service announcement in.
  // As the driver's level 5: channel 20, then 21, each with its walk (which posts receive buffers whenever it saw events), then
  // the buffers, then the hello.
  void bring()
  {
    start();
    CHECK(ipcrStart(transport, device.io(), kChanIpcrOut));
    turn();
    CHECK(ipcrRxPosted(transport) == 0);   // channel 21 is not running yet: nothing is posted into its ring
    CHECK(ipcrStart(transport, device.io(), kChanIpcrIn));
    turn();
    CHECK(ipcrRunning(transport));
    CHECK(ipcrRxPosted(transport) == kIpcrRxBuffers);   // the walk after 21's start posted them, once 21 ran
    CHECK(ipcrPostRx(transport, device.io()) == 0);
    qcom_qrtr_start(&router);
    for (unsigned i = 0; i < 8 && router.remote_count == 0; ++i) turn();
    CHECK(router.hello_received && router.remote_count == 1);
  }
  // Runs until the client is done, stopped, or nothing moves any more.
  int run(unsigned turns = 64)
  {
    for (unsigned i = 0; i < turns; ++i) {
      const unsigned before = device.doorbells + side->events;
      turn();
      if (client.step == kWlfwStepDone || client.step == kWlfwStepStopped) return client.step;
      if (device.doorbells + side->events == before && device.toHost.empty()) return client.step;
    }
    return client.step;
  }
};

static WlfwIo ioOf(Host &h)
{
  WlfwIo io;
  io.ctx = &h;
  io.claim = &Host::claim;
  io.send = &Host::sendMessage;
  io.file_size = &Host::fileSize;
  io.file_read = &Host::fileRead;
  io.memory = &Host::memory;
  io.step = &Host::step;
  io.event = &Host::event;
  return io;
}

// ======================================================================================================== the transport
static void testTransport()
{
  Host h;
  h.start();
  // The contexts as the chip reads them before the start: the fixed fields only (the MHI host's context initialisation), no ring.
  const uint8_t *out = mhiChannelCtx(h.contexts, kChanIpcrOut);
  const uint8_t *in = mhiChannelCtx(h.contexts, kChanIpcrIn);
  CHECK((get32(out) & 0xffu) == kChanStateDisabled && ((get32(out) >> 8) & 3u) == kChanBurstDisable);
  CHECK((get32(in) & 0xffu) == kChanStateDisabled && ((get32(in) >> 8) & 3u) == kChanBurstDisable);
  CHECK(get32(out + kChanType) == kChanTypeOutbound && get32(in + kChanType) == kChanTypeInbound);
  CHECK(get32(out + kChanErIndex) == kIpcrEventRing && get32(in + kChanErIndex) == kIpcrEventRing);
  CHECK(get64(out + kChanRBase) == 0 && get64(out + kChanRLen) == 0 && get64(in + kChanRBase) == 0);
  // Every other channel context is untouched.
  for (uint32_t chan = 0; chan < kMhiChannels; ++chan)
    if (chan != kChanIpcrOut && chan != kChanIpcrIn)
      for (uint32_t i = 0; i < kMhiContextStride; ++i) CHECK(mhiChannelCtx(h.contexts, chan)[i] == 0);
  // The doorbell arrays are inside the register space.
  CHECK(h.transport.chdb == h.device.chdb && h.transport.erdb == h.device.erdb && !h.transport.error);
  // A bare start command for a channel that was not prepared is refused by the chip (the model holds the MHI rule).
  CHECK(ipcrCommand(h.transport, h.device.io(), kChanIpcrOut, kTreCmdStart));
  h.turn();
  CHECK(!h.transport.started[0] && h.transport.commandErrors == 1);
  h.transport.commandErrors = 0;
  // The start: prepared (ring and enabled state), then the command, one at a time, 20 first.
  CHECK(ipcrStart(h.transport, h.device.io(), kChanIpcrOut));
  CHECK((get32(out) & 0xffu) == kChanStateEnabled && ((get32(out) >> 8) & 3u) == kChanBurstDisable);
  CHECK(get64(out + kChanRLen) == kIpcrElements * kMhiElement);
  CHECK(get64(out + kChanRBase) == h.transport.out.bus && get64(out + kChanRp) == h.transport.out.bus &&
        get64(out + kChanWp) == h.transport.out.bus);
  CHECK(h.transport.out.bus % (kIpcrElements * kMhiElement) == 0);   // the ring's base is a multiple of its length
  CHECK(!ipcrStart(h.transport, h.device.io(), kChanIpcrIn));           // one at a time
  CHECK(!ipcrStart(h.transport, h.device.io(), 7));                     // not one of the two
  h.turn();
  CHECK(h.transport.started[0] && !h.transport.started[1]);
  CHECK(ipcrStart(h.transport, h.device.io(), kChanIpcrIn));
  h.turn();
  CHECK(ipcrRunning(h.transport));
  CHECK(h.device.commandsSeen == 3 && h.transport.commands == 3 && h.transport.commandErrors == 0);
  CHECK(ipcrChanState(h.transport.out) == kChanStateRunning && ipcrChanState(h.transport.in) == kChanStateRunning);
  // The M0 answer: both event rings, and both running channels, rung again with what their contexts hold.
  const unsigned before = h.device.doorbells;
  CHECK(ipcrRingAll(h.transport, h.contexts, h.device.io()) == 4);
  CHECK(h.device.doorbells == before + 2);   // the model counts the channels' and the command ring's doorbells
  CHECK(h.device.reg64(h.device.erdb + 8) == get64(mhiEventCtx(h.contexts, 1) + 36));
  CHECK(h.device.reg64(h.device.chdb + 8 * kChanIpcrIn) == get64(in + kChanWp));
  // Every inbound element but one has a buffer (the turn above posted them), and a second posting adds none.
  CHECK(kIpcrRxBuffers == kIpcrElements - 1);
  unsigned busy = 0;
  for (const IpcrBuffer &b : h.transport.rx) busy += b.busy ? 1 : 0;
  CHECK(busy == kIpcrRxBuffers);
  CHECK(ipcrPostRx(h.transport, h.device.io()) == 0);
  // A packet out, a packet back.
  const int index = ipcrClaimTx(h.transport);
  CHECK(index == 0);
  std::memset(h.transport.tx[index].va, 0, 64);
  put32(h.transport.tx[index].va, 1);
  put32(h.transport.tx[index].va + 4, QCOM_QRTR_DATA);
  put32(h.transport.tx[index].va + 20, 4);
  put32(h.transport.tx[index].va + 28, 0x55);   // not the service's port: the model leaves it alone
  CHECK(ipcrSubmitTx(h.transport, h.device.io(), unsigned(index), QCOM_QRTR_HEADER + 4));
  CHECK(h.transport.tx[index].busy);
  h.firmware.queue(std::vector<uint8_t>(QCOM_QRTR_HEADER + 8, 0x5a));
  unsigned delivered = 0, freed = 0;
  h.side->run();
  ipcrPoll(h.transport, h.contexts, h.device.io(), [&](uint32_t kind, uint32_t, uint32_t code, uint32_t length, uint8_t *) {
    if (kind == kIpcrPacket) {
      ++delivered;
      CHECK(length == QCOM_QRTR_HEADER + 8 && code == kEvCodeEot);
    }
    if (kind == kIpcrTxDone) ++freed;
  });
  CHECK(delivered == 1 && freed == 1 && !h.transport.tx[index].busy);
  CHECK(h.transport.sent == 1 && h.transport.received == 1 && h.transport.txDone == 1 && h.transport.dropped == 0);
  // A state change and an execution-environment change reach the owner.
  unsigned states = 0, ees = 0;
  h.side->event(0, 0, kStateM0 << 24, kEvTypeStateChange << 16);
  h.side->event(0, 0, kEeAmss << 24, kEvTypeEeChange << 16);
  h.side->event(0, 0, 0, 0x7fu << 16);   // a type nobody knows: counted, never acted on
  ipcrPoll(h.transport, h.contexts, h.device.io(), [&](uint32_t kind, uint32_t, uint32_t, uint32_t, uint8_t *) {
    if (kind == kIpcrState) ++states;
    if (kind == kIpcrEe) ++ees;
  });
  CHECK(states == 1 && ees == 1 && h.transport.lastState == kStateM0 && h.transport.lastEe == kEeAmss);
  CHECK(h.transport.dropped == 1);
  // All four outbound buffers in flight: the fifth claim is refused, not queued.
  for (unsigned i = 0; i < kIpcrOutBuffers; ++i) {
    const int slot = ipcrClaimTx(h.transport);
    CHECK(slot >= 0);
    CHECK(ipcrSubmitTx(h.transport, h.device.io(), unsigned(slot), 64));
  }
  CHECK(ipcrClaimTx(h.transport) < 0 && h.transport.refused == 1);
  // A claim holds its buffer: the next claim is another one, and a claim given back is free again.
  Host fresh;
  fresh.bring();
  const int first = ipcrClaimTx(fresh.transport), second = ipcrClaimTx(fresh.transport);
  CHECK(first == 0 && second == 1 && fresh.transport.tx[0].busy && fresh.transport.tx[1].busy);
  ipcrReleaseTx(fresh.transport, unsigned(first));
  CHECK(!fresh.transport.tx[0].busy && ipcrClaimTx(fresh.transport) == 0);
  // A buffer that was never claimed is not submitted.
  CHECK(!ipcrSubmitTx(fresh.transport, fresh.device.io(), 3, 64));

  // An arena with no room for the rings is refused, and nothing of it is used.
  Host tiny;
  arenaInit(tiny.arena, tiny.device.memory.data(), kBusBase, 64 * 1024);
  uint64_t bus = 0;
  uint8_t *va = arenaAlloc(tiny.arena, kMhiContextBytes, &bus);
  CHECK(va && mhiContextsPlan(tiny.contexts, va, bus, kMhiContextBytes));
  CHECK(!ipcrPlan(tiny.transport, tiny.arena, tiny.contexts));
  // A chip whose doorbell offsets lie outside the register space is refused.
  Host bad;
  bad.start();
  bad.device.regs[kMhiChdbOff] = 0xfffff000u;
  CHECK(!ipcrOpen(bad.transport, bad.device.io(), bad.device.reglen) && bad.transport.error != nullptr);
}

// ================================================================== the rings after mission mode (desk-dev14, 2026-11-12)
// What the first boots that reached level 5 with a running firmware showed ("both channels run ... 0 receive buffers posted", no
// hello from the chip, ours unanswered), and what the transport now holds to.
static void testRings()
{
  // The boot's order: channel 20, its walk (which posts after any event), channel 21, its walk. Nothing goes into 21's ring before
  // its start completed, and after it every buffer is on the ring, which the chip reads up to the doorbell.
  {
    Host h;
    h.start();
    h.side->helloFirst = true;   // the chip's router speaks first, as soon as it has somewhere to put the hello
    CHECK(ipcrStart(h.transport, h.device.io(), kChanIpcrOut));
    h.turn();
    CHECK(h.transport.started[0] && !h.transport.started[1]);
    CHECK(ipcrPostRx(h.transport, h.device.io()) == 0 && ipcrRxPosted(h.transport) == 0);
    CHECK(h.side->staleDoorbells == 0);   // not even a doorbell for the channel that is not running
    CHECK(ipcrStart(h.transport, h.device.io(), kChanIpcrIn));
    h.turn();
    CHECK(ipcrRunning(h.transport) && ipcrRxPosted(h.transport) == kIpcrRxBuffers);
    const uint8_t *in = mhiChannelCtx(h.contexts, kChanIpcrIn);
    const uint64_t wp = h.transport.in.bus + uint64_t(kIpcrRxBuffers) * kMhiElement;
    CHECK(get64(in + kChanWp) == wp && h.device.reg64(h.device.chdb + 8 * kChanIpcrIn) == wp && h.transport.in.doorbells == 1);
    for (uint32_t e = 0; e < kIpcrRxBuffers; ++e) {   // each element: its buffer, 8 KiB, a transfer asking for its completion
      const uint8_t *element = h.transport.in.va + e * kMhiElement;
      CHECK(h.transport.in.slot[e] >= 0 && get64(element) == h.transport.rx[h.transport.in.slot[e]].bus);
      CHECK((get32(element + 8) & 0xffffu) == kIpcrBufferBytes && get32(element + 12) == (kTreTypeTransfer << 16 | kTreIeot));
    }
    CHECK(h.transport.in.slot[kIpcrRxBuffers] == kIpcrNoBuffer);   // one element stays free
    // The chip's hello comes in on the next walk, is answered, and its announcement follows our answer.
    for (unsigned i = 0; i < 6 && h.router.remote_count == 0; ++i) h.turn();
    CHECK(h.router.hello_received && h.router.hello_sent && h.router.remote_count == 1 && h.router.remote_node == kFirmwareNode);
    CHECK(h.transport.received >= 2 && h.transport.dropped == 0 && h.transport.skipped == 0);
    // Our answer went out on 20 and its completion (naming the element, not the buffer) gave the buffer back.
    CHECK(h.transport.sent >= 1 && h.transport.txDone == h.transport.sent && h.transport.out.rp == h.transport.out.wp);
    for (const IpcrBuffer &b : h.transport.tx) CHECK(!b.busy);
    // Every received buffer went back on the ring: the chip still holds all but one element's worth.
    CHECK(ipcrRxPosted(h.transport) == kIpcrRxBuffers && ipcrOutstanding(h.transport.in) == kIpcrRxBuffers);
  }
  // Many more packets than the ring has elements: the ring wraps, every packet is delivered once, the buffers keep coming back.
  {
    Host h;
    h.bring();
    const unsigned before = h.transport.received;
    for (unsigned i = 0; i < 3 * kIpcrElements; ++i) {
      std::vector<uint8_t> packet(QCOM_QRTR_HEADER + 4, 0);
      put32(packet.data(), 1);
      put32(packet.data() + 4, QCOM_QRTR_DATA);
      put32(packet.data() + 20, 4);
      put32(packet.data() + 28, 0x77);   // nobody's port: counted as unrouted by the router
      h.firmware.queue(packet);
      if (i % 5 == 4) h.turn();
    }
    for (unsigned i = 0; i < 8; ++i) h.turn();
    CHECK(h.transport.received - before == 3 * kIpcrElements && h.router.rx_unrouted == 3 * kIpcrElements);
    CHECK(h.transport.dropped == 0 && h.transport.skipped == 0 && ipcrRxPosted(h.transport) == kIpcrRxBuffers);
  }
  // A completion that names a buffer's address (what the old walk expected) is not an element of the ring: dropped, the
  // buffer stays the chip's.
  {
    Host h;
    h.bring();
    const unsigned dropped = h.transport.dropped, received = h.transport.received;
    const int slot = h.transport.in.slot[h.transport.in.rp];
    CHECK(slot >= 0);
    h.side->event(kIpcrEventRing, h.transport.rx[slot].bus, kEvCodeEot << 24 | 40, kChanIpcrIn << 24 | kEvTypeTransfer << 16);
    h.turn();
    CHECK(h.transport.dropped == dropped + 1 && h.transport.received == received && h.transport.rx[slot].busy);
    // nor is an element the chip does not hold (the free one past the write pointer)
    h.side->event(kIpcrEventRing, h.transport.in.bus + uint64_t(h.transport.in.wp) * kMhiElement, kEvCodeEot << 24 | 40,
                  kChanIpcrIn << 24 | kEvTypeTransfer << 16);
    h.turn();
    CHECK(h.transport.dropped == dropped + 2 && h.transport.received == received);
  }
  // A chip that merges two completions into one: the named element is delivered, the one before it given back unread.
  {
    Host h;
    h.bring();
    h.side->mergeEvents = true;
    const unsigned received = h.transport.received;
    for (unsigned i = 0; i < 2; ++i) h.firmware.queue(std::vector<uint8_t>(QCOM_QRTR_HEADER + 8, 0x11));
    h.turn();
    h.turn();
    CHECK(h.transport.received == received + 1 && h.transport.skipped == 1 && ipcrRxPosted(h.transport) == kIpcrRxBuffers);
  }
  // A packet larger than the buffer: the chip reports an overflow; nothing delivered, the buffer back on the ring.
  {
    Host h;
    h.bring();
    const unsigned received = h.transport.received;
    h.firmware.queue(std::vector<uint8_t>(kIpcrBufferBytes + 64, 0x22));
    h.turn();
    h.turn();
    CHECK(h.transport.received == received && h.transport.transferErrors == 1 && h.transport.skipped == 1);
    CHECK(h.transport.in.lastCode == kEvCodeOverflow && ipcrRxPosted(h.transport) == kIpcrRxBuffers);
  }
  // The channel back in doorbell mode: the packet is not taken until the host rings again, which the walk does.
  {
    Host h;
    h.bring();
    h.side->dbModeOnce = true;
    const int index = ipcrClaimTx(h.transport);
    CHECK(index >= 0);
    std::memset(h.transport.tx[index].va, 0, 64);
    put32(h.transport.tx[index].va, 1);
    put32(h.transport.tx[index].va + 4, QCOM_QRTR_DATA);
    put32(h.transport.tx[index].va + 28, 0x55);
    const unsigned taken = h.device.packetsTaken, rung = h.transport.out.doorbells;
    CHECK(ipcrSubmitTx(h.transport, h.device.io(), unsigned(index), QCOM_QRTR_HEADER + 4));
    h.turn();   // the chip answers with a doorbell-mode event; the walk rings the channel again
    CHECK(h.transport.dbModes == 1 && h.transport.out.doorbells == rung + 2 && h.transport.tx[index].busy);
    h.turn();   // and now it is taken and completed
    CHECK(h.device.packetsTaken == taken + 1 && !h.transport.tx[index].busy);
  }
  // A send on a channel that does not run is refused and its claim given back; a restarted inbound channel starts empty.
  {
    Host h;
    h.start();
    const int index = ipcrClaimTx(h.transport);
    CHECK(index >= 0 && !ipcrSubmitTx(h.transport, h.device.io(), unsigned(index), 64) && !h.transport.tx[index].busy);
    Host r;
    r.bring();
    CHECK(ipcrRxPosted(r.transport) == kIpcrRxBuffers);
    CHECK(ipcrStart(r.transport, r.device.io(), kChanIpcrIn));
    CHECK(!r.transport.started[1] && ipcrRxPosted(r.transport) == 0 && r.transport.in.wp == 0 && r.transport.in.rp == 0);
    CHECK(ipcrPostRx(r.transport, r.device.io()) == 0);   // not before the start completed
    r.turn();
    CHECK(r.transport.started[1] && ipcrRxPosted(r.transport) == kIpcrRxBuffers);
  }
}

// ====================================================================================================== the router's copy
static void testRouter()
{
  Host h;
  h.bring();
  CHECK(h.router.remote_node == kFirmwareNode);
  uint32_t node = 0, port = 0;
  CHECK(qcom_qrtr_find_service(&h.router, kWlfwService, kWlfwInstanceWord, &node, &port) == 0);
  CHECK(node == kFirmwareNode && port == kFirmwarePort);
  CHECK(qcom_qrtr_find_service(&h.router, kWlfwService, 0x999, nullptr, nullptr) == -1);
  CHECK(qcom_qrtr_find_remote(&h.router, kWlfwService) >= 0);
  // The addition: a packet built in a transport buffer, with the header written in front of it.
  const int index = ipcrClaimTx(h.transport);
  CHECK(index >= 0);
  uint8_t *buffer = h.transport.tx[index].va;
  const char *message = "hello";
  std::memcpy(buffer + QCOM_QRTR_HEADER, message, 5);
  CHECK(qcom_qrtr_send_buffer(&h.router, buffer, h.transport.tx[index].bytes, kQrtrClientPort, node, port, 5) == 0);
  CHECK(get32(buffer) == 1 && get32(buffer + 4) == QCOM_QRTR_DATA && get32(buffer + 8) == kQrtrLocalNode);
  CHECK(get32(buffer + 12) == kQrtrClientPort && get32(buffer + 20) == 5 && get32(buffer + 24) == node &&
        get32(buffer + 28) == port);
  CHECK(buffer[QCOM_QRTR_HEADER + 5] == 0 && buffer[QCOM_QRTR_HEADER + 7] == 0);   // the padding is zeroed
  // A payload that does not fit the buffer is refused, not truncated.
  CHECK(qcom_qrtr_send_buffer(&h.router, buffer, 64, kQrtrClientPort, node, port, 4096) == -1);

  // The flow-control answer (P21): a data packet from the firmware's port 1 on node 7 to our port that asks for one gets a
  // RESUME_TX from our port to that node:port, naming *our* node and port (the destination the sender resumes) — not the sender's.
  {
    struct Capture {
      std::vector<uint8_t> last;
      unsigned sent = 0, refuse = 0;
    } cap;
    QcomQrtr r;
    qcom_qrtr_init(&r, kQrtrLocalNode, [](void *ctx, const uint8_t *p, uint32_t n) {
      Capture &c = *static_cast<Capture *>(ctx);
      if (c.refuse) return --c.refuse, -1;
      c.last.assign(p, p + n);
      ++c.sent;
      return 0;
    }, &cap);
    CHECK(qcom_qrtr_add_client(&r, kQrtrClientPort, [](void *, QcomQrtr *, uint32_t, uint32_t, uint32_t, const uint8_t *,
                                                       uint32_t) {}, nullptr) == 0);
    std::vector<uint8_t> data(QCOM_QRTR_HEADER + 8, 0);
    put32(data.data(), 1);
    put32(data.data() + 4, QCOM_QRTR_DATA);
    put32(data.data() + 8, 7);                 // from node 7
    put32(data.data() + 12, 1);                // port 1 (the WLAN service's, as on the tablet)
    put32(data.data() + 16, 1);                // confirm_rx
    put32(data.data() + 20, 7);
    put32(data.data() + 24, kQrtrLocalNode);
    put32(data.data() + 28, kQrtrClientPort);
    CHECK(qcom_qrtr_receive(&r, data.data(), uint32_t(data.size())) == 0);
    CHECK(r.rx_confirms == 1 && r.resumes_sent == 1 && !r.resume_owed && cap.sent == 1);
    CHECK(cap.last.size() == QCOM_QRTR_HEADER + 20);
    const uint8_t *p = cap.last.data();
    CHECK(get32(p) == 1 && get32(p + 4) == QCOM_QRTR_RESUME_TX);
    CHECK(get32(p + 8) == kQrtrLocalNode && get32(p + 12) == kQrtrClientPort);   // from our port
    CHECK(get32(p + 16) == 0 && get32(p + 20) == 20);                            // no confirmation asked, 20 bytes
    CHECK(get32(p + 24) == 7 && get32(p + 28) == 1);                             // to the sender's node:port
    CHECK(get32(p + 32) == QCOM_QRTR_RESUME_TX);
    CHECK(get32(p + 36) == kQrtrLocalNode && get32(p + 40) == kQrtrClientPort);  // naming us, the flow's destination
    // One the link refuses is owed, and sent by the next poll; a poll with nothing owed sends nothing.
    cap.refuse = 1;
    CHECK(qcom_qrtr_receive(&r, data.data(), uint32_t(data.size())) == 0);
    CHECK(r.rx_confirms == 2 && r.resumes_sent == 1 && r.resume_owed && cap.sent == 1 && r.tx_refused == 1);
    CHECK(qcom_qrtr_poll(&r) == 1 && !r.resume_owed && r.resumes_sent == 2 && cap.sent == 2);
    CHECK(get32(cap.last.data() + 24) == 7 && get32(cap.last.data() + 28) == 1 && get32(cap.last.data() + 40) == kQrtrClientPort);
    CHECK(qcom_qrtr_poll(&r) == 0 && cap.sent == 2);
    // A data packet that asks for nothing gets nothing.
    put32(data.data() + 16, 0);
    CHECK(qcom_qrtr_receive(&r, data.data(), uint32_t(data.size())) == 0 && r.rx_confirms == 2 && cap.sent == 2);
  }

  // The protocol's version 2 header (16 bytes, 16-bit nodes and ports, 0xffff the control port, optional words after it), which a
  // remote may use while this side sends version 1: a hello, an announcement and a data packet are read as their version 1 forms.
  QcomQrtr v2;
  unsigned v2Data = 0;
  uint32_t v2From = 0, v2Length = 0;
  qcom_qrtr_init(&v2, kQrtrLocalNode, [](void *, const uint8_t *, uint32_t) { return 0; }, nullptr);
  CHECK(qcom_qrtr_add_client(&v2, kQrtrClientPort, [](void *ctx, QcomQrtr *, uint32_t, uint32_t, uint32_t from, const uint8_t *,
                                                      uint32_t length) {
          auto *seen = static_cast<std::pair<unsigned *, std::pair<uint32_t *, uint32_t *>> *>(ctx);
          ++*seen->first;
          *seen->second.first = from;
          *seen->second.second = length;
        }, nullptr) == 0);
  std::pair<unsigned *, std::pair<uint32_t *, uint32_t *>> seen{&v2Data, {&v2From, &v2Length}};
  v2.local[0].context = &seen;
  auto v2Packet = [](uint8_t type, uint8_t flags, uint8_t optWords, uint16_t srcNode, uint16_t srcPort, uint16_t dstNode,
                     uint16_t dstPort, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> p(16 + 4 * optWords + payload.size(), 0);
    p[0] = 3;
    p[1] = type;
    p[2] = flags;
    p[3] = optWords;
    put32(p.data() + 4, uint32_t(payload.size()));
    p[8] = uint8_t(srcNode), p[9] = uint8_t(srcNode >> 8), p[10] = uint8_t(srcPort), p[11] = uint8_t(srcPort >> 8);
    p[12] = uint8_t(dstNode), p[13] = uint8_t(dstNode >> 8), p[14] = uint8_t(dstPort), p[15] = uint8_t(dstPort >> 8);
    std::memcpy(p.data() + 16 + 4 * optWords, payload.data(), payload.size());
    return p;
  };
  std::vector<uint8_t> hello(20, 0);
  put32(hello.data(), QCOM_QRTR_HELLO);
  CHECK(qcom_qrtr_receive(&v2, v2Packet(QCOM_QRTR_HELLO, 0, 0, 7, 0xffff, 0xffff, 0xffff, hello).data(), 36) == 0);
  CHECK(v2.hello_received && v2.remote_node == 7 && v2.hello_sent);
  std::vector<uint8_t> server(20, 0);
  put32(server.data(), QCOM_QRTR_NEW_SERVER);
  put32(server.data() + 4, kWlfwService);
  put32(server.data() + 8, kWlfwInstanceWord);
  put32(server.data() + 12, 7);
  put32(server.data() + 16, 0x1d);
  const std::vector<uint8_t> announce = v2Packet(QCOM_QRTR_NEW_SERVER, 0, 1, 7, 0xffff, 1, 0xffff, server);   // one optional word
  CHECK(qcom_qrtr_receive(&v2, announce.data(), uint32_t(announce.size())) == 0);
  uint32_t n2 = 0, p2 = 0;
  CHECK(qcom_qrtr_find_service(&v2, kWlfwService, kWlfwInstanceWord, &n2, &p2) == 0 && n2 == 7 && p2 == 0x1d);
  const std::vector<uint8_t> data = v2Packet(QCOM_QRTR_DATA, 1, 0, 7, 0x1d, 1, uint16_t(kQrtrClientPort), {1, 2, 3, 4, 5});
  const uint32_t resumes = v2.resumes_sent;
  CHECK(qcom_qrtr_receive(&v2, data.data(), uint32_t(data.size())) == 0);
  CHECK(v2Data == 1 && v2From == 0x1d && v2Length == 5 && v2.resumes_sent == resumes + 1);   // flags bit 0: RESUME_TX answered
  // A version 2 header that claims more than the packet holds, and a version nobody has, are refused.
  std::vector<uint8_t> lying = v2Packet(QCOM_QRTR_DATA, 0, 0, 7, 0x1d, 1, uint16_t(kQrtrClientPort), {1, 2, 3, 4});
  put32(lying.data() + 4, 400);
  CHECK(qcom_qrtr_receive(&v2, lying.data(), uint32_t(lying.size())) == -1);
  lying[0] = 2;
  CHECK(qcom_qrtr_receive(&v2, lying.data(), uint32_t(lying.size())) == -1 && v2.rx_bad == 2);
}

// ========================================================================================================= the WLFW client
static void testClient()
{
  Host h;
  h.bring();
  const WlfwIo io = ioOf(h);
  CHECK(wlfwStart(h.client, io, 1, 1));   // piano-wifi-mode=1 piano-wifi-shadow=1: the whole exchange
  CHECK(h.run() == kWlfwStepDone);
  CHECK(wlfwFinished(h.client) && h.client.error == kWlfwErrNone);
  // What the firmware told us.
  CHECK(h.client.radios == 2 && h.client.singleChipMlo && h.client.boardId == 0x21);
  CHECK(h.client.chipId == 0x20 && h.client.chipFamily == 0x4b && h.client.socId == 0x400);
  CHECK(h.client.fwVersion == 0x301 && std::string(h.client.buildTimestamp) == "2026-01-01 00:00");
  CHECK(h.client.macs == 1 && h.client.voltageMv == 950 && h.client.otpVersion == 0x11 && h.client.fwCaps == 0x81);
  CHECK(h.client.fwStatus == 0x1234);
  // The memory the firmware asked for, and what it got.
  CHECK(h.firmware.memRequests == 1 && h.firmware.respondEmpty == 0);
  CHECK(h.client.regionCount == 4 && h.client.regionBytes == (1u << 20) + (512u << 10) + (64u << 10) + (32u << 10));
  CHECK(h.client.regionGiven == (1u << 20) + (512u << 10) + (64u << 10));   // the board-file region is not ours to answer
  CHECK(h.client.regions[3].address == 0 && h.client.regions[3].type == kMemBdf);
  // The downloads: the whole files, in segments of at most 6 KiB, and nothing else.
  CHECK(h.firmware.regdb == h.files["regdb_xiaomi.bin"] && h.firmware.regdbSegments == 3);
  CHECK(h.firmware.board == h.files["bd_p81.elf"] && h.firmware.boardSegments == 1);
  CHECK(h.firmware.qdss == h.files["qdss_trace_config_v2.cfg"] && h.firmware.qdssSegments == 1);
  CHECK(h.firmware.m3Bytes == h.files["phy_ucode20.elf"].size());
  CHECK(h.firmware.auxBytes == h.files["aux_ucode20.elf"].size());
  // The two microcodes are in the window, where the chip reads them.
  const uint8_t *m3 = h.device.at(h.firmware.m3Address, h.firmware.m3Bytes);
  CHECK(m3 && std::memcmp(m3, h.files["phy_ucode20.elf"].data(), h.firmware.m3Bytes) == 0);
  const uint8_t *aux = h.device.at(h.firmware.auxAddress, h.firmware.auxBytes);
  CHECK(aux && std::memcmp(aux, h.files["aux_ucode20.elf"].data(), h.firmware.auxBytes) == 0);
  // The configuration and the mode.
  CHECK(h.firmware.iniSeen && h.firmware.cfgSeen && h.firmware.ready);
  CHECK(h.firmware.pipes == kCePipeCount && h.firmware.services == kCeServiceCount);
  CHECK(h.firmware.shadow.size() == h.client.shadowCount && h.client.shadowCount == kShadowRegisters);
  CHECK(h.firmware.mode == kFirmwareModeNormal);
  CHECK(h.client.unexpected == 0 && h.sendRefusals == 0);
  CHECK(h.client.requests == h.client.responses);
  // Every step was announced once, in order, and the last one is the mode.
  CHECK(h.lastStep == kWlfwStepDone);
  for (unsigned i = 0; i < 4; ++i) h.turn();   // the last flow-control answer reaches the firmware's model
  // The firmware's router asked for flow-control answers (more than ten packets came to our port) and every one resumed it.
  CHECK(h.firmware.dataSent > Firmware::kFlowHigh && h.firmware.confirmsAsked >= 2 && h.firmware.held.empty());
  CHECK(h.firmware.resumes == h.firmware.confirmsAsked && h.firmware.misaddressedResumes == 0);
  CHECK(h.router.rx_confirms == h.firmware.confirmsAsked && h.router.resumes_sent == h.firmware.confirmsAsked);
  // The owner heard every response with its codes and every indication, the downloads with their segments.
  unsigned responses = 0, indications = 0, regdbSegments = 0;
  for (const WlfwEvent &e : h.events) {
    CHECK(e.kind != kWlfwEventUnexpected);
    if (e.kind == kWlfwEventIndication) {
      ++indications;
      continue;
    }
    ++responses;
    CHECK(e.hasResult && e.result == kQmiResultSuccess && e.error == 0);
    if (e.message == kWlfwBdfDownload && e.file && std::strcmp(e.file, kRegdbName) == 0) {
      CHECK(e.segment == int32_t(regdbSegments) && e.segments == 3 && e.last == (regdbSegments == 2));
      CHECK(e.bytes == (regdbSegments < 2 ? kWlfwMaxDataBytes : 13000 - 2 * kWlfwMaxDataBytes));
      ++regdbSegments;
    }
    if (e.message != kWlfwBdfDownload && e.message != kWlfwQdssDownload) CHECK(e.segment == -1);
  }
  CHECK(responses == h.client.responses && indications == h.client.indications && regdbSegments == 3);
  CHECK(h.client.havePhyBoard && h.client.phyBoardId == 0x21 && h.client.haveCapBoard && h.client.capBoardId == 0x21);
}

// P21 (2026-11-12): the tablet's exchange with the stock files' sizes. The firmware's router stops sending to our port after its
// tenth packet until a RESUME_TX names our port; the copied router's answer named the sender, so on the tablet the answer to the
// regulatory database's fourth and last segment never came: 9 requests, 8 responses, 2 indications, 4 download segments, a step
// timed out. A link that loses every answer reproduces exactly that; the fixed answer takes the exchange to the firmware's ready.
static void stockFiles(Host &h)
{
  h.files["regdb_xiaomi.bin"] = std::string(24278, 'r');        // 4 segments, the last 5846 bytes
  h.files["bd_p81.elf"] = std::string(125272, 'b');             // 21 segments
  h.files["phy_ucode20.elf"] = std::string(290816, 'm');
  h.files["aux_ucode20.elf"] = std::string(61440, 'a');
  h.files["qdss_trace_config_v2.cfg"] = std::string(6241, 'q');  // 2 segments
}

static void testFlowControl()
{
  {   // the tablet's failure, reproduced: every flow-control answer lost
    Host h;
    h.bring();
    stockFiles(h);
    h.refuseResumes = 2;
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    CHECK(h.run(256) == kWlfwStepRegdb);
    CHECK(h.client.waiting && h.client.requests == 9 && h.client.responses == 8 && h.client.indications == 2);
    CHECK(h.client.segments == 4 && h.firmware.regdbSegments == 4);   // the chip has the whole file
    CHECK(h.firmware.dataSent == Firmware::kFlowHigh && h.firmware.held.size() == 1 && h.router.rx_data == 10);
    CHECK(h.client.outSegment == 3 && h.client.outSegments == 4 && h.client.outBytes == 24278 - 3 * kWlfwMaxDataBytes &&
          h.client.outLast && h.client.pendingMessage == kWlfwBdfDownload);
    CHECK(h.router.resume_owed && h.router.rx_confirms == 1 && h.router.resumes_sent == 0);
    CHECK(wlfwTimedOut(h.client) == kWlfwStepStopped && h.client.timedOutWaiting && h.client.failedStep == kWlfwStepRegdb);
  }
  {   // the old answer (naming the sender, from the control port) resumes nothing: the firmware's model holds it to the protocol
    Host h;
    h.bring();
    stockFiles(h);
    h.refuseResumes = 2;
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    CHECK(h.run(256) == kWlfwStepRegdb);
    std::vector<uint8_t> old(QCOM_QRTR_HEADER + 20, 0);
    put32(old.data(), 1);
    put32(old.data() + 4, QCOM_QRTR_RESUME_TX);
    put32(old.data() + 8, kQrtrLocalNode);
    put32(old.data() + 12, QCOM_QRTR_PORT_CTRL);
    put32(old.data() + 20, 20);
    put32(old.data() + 24, kFirmwareNode);
    put32(old.data() + 28, QCOM_QRTR_PORT_CTRL);
    put32(old.data() + QCOM_QRTR_HEADER, QCOM_QRTR_RESUME_TX);
    put32(old.data() + QCOM_QRTR_HEADER + 4, kFirmwareNode);
    put32(old.data() + QCOM_QRTR_HEADER + 8, kFirmwarePort);
    h.firmware.onResume(old);
    CHECK(h.firmware.misaddressedResumes == 1 && h.firmware.held.size() == 1);
    // and the right one, once the link takes it again, lets the held answer out and the exchange run to the end
    h.refuseResumes = 0;
    CHECK(h.run(512) == kWlfwStepDone);
    CHECK(h.firmware.resumes >= 1 && h.firmware.held.empty() && h.client.error == kWlfwErrNone);
  }
  {   // the fix: the stock sizes run through, a first lost answer is sent again by the poll
    Host h;
    h.bring();
    stockFiles(h);
    h.refuseResumes = 1;
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    CHECK(h.run(512) == kWlfwStepDone);
    CHECK(h.client.error == kWlfwErrNone && h.resumeRefusals == 1 && !h.router.resume_owed);
    CHECK(h.firmware.regdbSegments == 4 && h.firmware.boardSegments == 21 && h.firmware.qdssSegments == 2);
    CHECK(h.firmware.regdb == h.files["regdb_xiaomi.bin"] && h.firmware.board == h.files["bd_p81.elf"]);
    for (unsigned i = 0; i < 4; ++i) h.turn();   // the last answer reaches the firmware's model
    CHECK(h.firmware.confirmsAsked == h.firmware.resumes && h.firmware.misaddressedResumes == 0 && h.firmware.held.empty());
    CHECK(h.firmware.dataSent == h.client.responses + h.client.indications);
  }
  {   // this unit's board identifier: the chip names none (0xff, also in the radio's capability); every download carries 0xff and
      // the board file is the stock API-1 default, as the chip's own driver chooses
    gSimPhyBoard = 0xff;
    gSimBoard = 0xff;
    Host h;
    h.bring();
    stockFiles(h);
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    CHECK(h.run(512) == kWlfwStepDone);
    CHECK(h.client.boardId == 0xff && h.firmware.lastFileId == 0xff && h.client.phyBoardId == 0xff);
    const char *why = nullptr;
    CHECK(std::strcmp(wlfwBoardFile(h.client.boardId, &why), "bd_p81.elf") == 0 && why && std::strstr(why, "0xff"));
    gSimPhyBoard = gSimBoard = 0x21;
  }
  {   // a chip capability without board information: 0xff, whatever the radio's capability said (it is only logged)
    gSimCapBoard = false;
    Host h;
    h.bring();
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    CHECK(h.run(256) == kWlfwStepDone);
    CHECK(h.client.boardId == kBoardIdDefault && !h.client.haveCapBoard && h.client.havePhyBoard && h.client.phyBoardId == 0x21);
    CHECK(h.firmware.lastFileId == kBoardIdDefault);
    gSimCapBoard = true;
  }
  {   // the trace configuration refused: noted, the rest of it not sent, and the firmware's ready still awaited and taken
    Host h;
    h.bring();
    stockFiles(h);
    h.firmware.refuseQdss = true;
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    CHECK(h.run(512) == kWlfwStepDone);
    CHECK(h.client.qdssRefused && h.client.qdssResult == 1 && h.client.qdssError == 0x2f && h.firmware.qdssSegments == 1);
  }
  {   // no trace configuration on the partition: skipped
    Host h;
    h.bring();
    h.files.erase("qdss_trace_config_v2.cfg");
    h.firmware.earlyFwReady = false;
    CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
    // the simulated firmware says "ready" after the trace configuration's last segment; without one it never would: say it here
    for (unsigned i = 0; i < 256 && h.client.step != kWlfwStepFwReady && h.client.step != kWlfwStepStopped; ++i) h.turn();
    CHECK(h.client.step == kWlfwStepFwReady && h.client.qdssSkipped && h.firmware.qdssSegments == 0);
    h.firmware.indicate(kWlfwFwReadyInd);
    CHECK(h.run() == kWlfwStepDone);
  }
}

static void testClientGate()
{
  // Without piano-wifi-mode the client stops at the ready indication: nothing of the data path is configured.
  Host h;
  h.bring();
  CHECK(wlfwStart(h.client, ioOf(h), 0, 0));
  CHECK(h.run() == kWlfwStepDone);
  CHECK(!h.firmware.iniSeen && !h.firmware.cfgSeen && h.firmware.mode == 0xffffffffu);
  CHECK(h.client.shadowCount == 0);
}

// A link that is up with the firmware's service announced: the common ground of the fault tests.
static Host *linked()
{
  Host *h = new Host();
  h->bring();
  return h;
}

static void testClientFaults()
{
  {   // a refused request ends level 5 at that step
    Host *h = linked();
    h->firmware.refuseHostCap = true;
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run() == kWlfwStepStopped);
    CHECK(h->client.error == kWlfwErrResult && h->client.failedStep == kWlfwStepHostCap && h->client.result == 1);
    delete h;
  }
  {   // a refused download, in the middle of a file
    Host *h = linked();
    h->firmware.refuseBoard = true;
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run() == kWlfwStepStopped);
    CHECK(h->client.error == kWlfwErrResult && h->client.failedStep == kWlfwStepBoard);
    delete h;
  }
  {   // a firmware file that is not on the partition
    Host *h = linked();
    h->files.erase("aux_ucode20.elf");
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run() == kWlfwStepStopped);
    CHECK(h->client.error == kWlfwErrFile && h->client.failedStep == kWlfwStepAuxUc);
    delete h;
  }
  {   // a partition that stops answering in the middle of a download
    Host *h = linked();
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    for (unsigned i = 0; i < 64 && h->client.step != kWlfwStepRegdb; ++i) h->turn();
    h->fileFails = true;
    CHECK(h->run() == kWlfwStepStopped);
    CHECK(h->client.error == kWlfwErrFile);
    delete h;
  }
  {   // a region larger than the window: the empty form, then the smaller request answered
    Host *h = linked();
    h->firmware.bigRegion = 32u << 20;   // more than the window the test gives
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run(128) == kWlfwStepDone);
    CHECK(h->firmware.respondEmpty == 1 && h->firmware.memRequests == 2 && h->client.memRequests == 2);
    CHECK(!h->client.memDelayed && h->client.regionCount == 2);
    delete h;
  }
  {   // a region this driver must answer, smaller than the service's chunk limit (so there is no "ask again" fallback) and no
      // room left in the window: the level stops instead of leaving the firmware without the memory it asked for
    Host *h = linked();
    h->arena.used = h->arena.bytes;   // nothing of the window is left
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run() == kWlfwStepStopped);
    CHECK(h->client.error == kWlfwErrMemory && h->client.failedStep == kWlfwStepMemRequest);
    delete h;
  }
  {   // an answer with the wrong transaction is counted and ignored, and the step then times out
    Host *h = linked();
    h->firmware.wrongTransaction = true;
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run() == kWlfwStepPhyCap);
    CHECK(h->client.unexpected >= 1 && h->client.waiting);
    CHECK(wlfwTimedOut(h->client) == kWlfwStepStopped && h->client.error == kWlfwErrTimeout &&
          h->client.failedStep == kWlfwStepPhyCap);
    delete h;
  }
  {   // the three indications arriving before the answers they follow: each is kept and the waiting step finds it there
    for (unsigned which = 0; which < 3; ++which) {
      Host *h = linked();
      h->firmware.earlyMemRequest = which == 0;
      h->firmware.earlyMemReady = which == 1;
      h->firmware.earlyFwReady = which == 2;
      CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
      CHECK(h->run(128) == kWlfwStepDone);
      CHECK(h->client.error == kWlfwErrNone && h->client.unexpected == 0);
      CHECK(!h->client.pendingMemRequest && !h->client.pendingMemReady && !h->client.pendingFwReady);
      delete h;
    }
  }
  {   // an indication in the wrong step is counted, never acted on
    Host *h = linked();
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    h->turn();                                  // the first request is out: the firmware knows our port
    h->firmware.indicate(kWlfwFwReadyInd);      // and now says "ready" far too early
    for (unsigned i = 0; i < 4; ++i) h->turn();
    CHECK(h->client.unexpected >= 1 && h->client.step != kWlfwStepDone && h->client.step != kWlfwStepStopped);
    delete h;
  }
  {   // a firmware that refuses to say how many radios it has: not a failure
    Host *h = linked();
    h->firmware.answerPhyCap = false;
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(h->run() == kWlfwStepDone);
    CHECK(h->client.radios == 0 && h->client.error == kWlfwErrNone);   // the owner then uses the chip's default
    delete h;
  }
  {   // a second exchange in one boot is refused
    Host *h = linked();
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    CHECK(!wlfwStart(h->client, ioOf(*h), 0, 0) && h->client.error == kWlfwErrState);
    delete h;
  }
  {   // a message that does not parse
    Host *h = linked();
    CHECK(wlfwStart(h->client, ioOf(*h), 0, 0));
    const uint8_t rubbish[3] = {kQmiResponse, 1, 0};
    CHECK(wlfwReceive(h->client, rubbish, sizeof rubbish) == kWlfwStepStopped);
    CHECK(h->client.error == kWlfwErrMessage);
    delete h;
  }
}

static void testShadowList()
{
  uint32_t list[kShadowRegisters] = {};
  const unsigned n = shadowList(list, kShadowRegisters, kShadowRingsPlanned, kShadowRingsPlannedCount);
  CHECK(n == kShadowRegisters);                       // the chip has 40 of them: the list fills up
  CHECK(list[0] == kUnitReo + kReo1RingHp + kShadowHpToTp);        // a destination ring shadows its tail pointer
  CHECK(list[1] == kUnitReo + kReo2RingHp + kShadowHpToTp);
  CHECK(list[7] == kUnitReo + kReo1RingHp + 7 * (kReo2RingHp - kReo1RingHp) + kShadowHpToTp);
  CHECK(list[8] == kUnitReo + kReoSw0RingHp + kShadowHpToTp);
  CHECK(list[9] == kUnitReo + kSw2ReoHp);                          // a source ring shadows its head pointer
  // A smaller capacity stops the list where it runs out, and a ring list of none gives none.
  uint32_t few[4] = {};
  CHECK(shadowList(few, 4, kShadowRingsPlanned, kShadowRingsPlannedCount) == 4);
  CHECK(shadowList(list, kShadowRegisters, kShadowRingsPlanned, 0) == 0);
  // Never more than the chip's own count, whatever the capacity says.
  uint32_t many[kShadowMax] = {};
  CHECK(shadowList(many, kShadowMax, kShadowRingsPlanned, kShadowRingsPlannedCount) == kShadowRegisters);
}

// ===================================================================================== the 32-bit window for stream 0x1401
struct SmmuModel {
  std::map<uint32_t, uint32_t> regs;
  unsigned groups = 128, banks = 83, pages = 128;
  SmmuModel()
  {
    regs[iova::kIdr0] = iova::kIdr0S1ts | groups;
    regs[iova::kIdr1] = (6u << 28) | banks;    // 128 pages, 83 banks
    regs[iova::kIdr2] = iova::kIdr2Ptfs4k | (2u << 4);   // 40-bit output
  }
  uint32_t rd(uint32_t off) { return regs.count(off) ? regs[off] : 0; }
  void wr(uint32_t off, uint32_t value) { regs[off] = value; }
  void occupy(unsigned slot, uint16_t stream, unsigned bank)
  {
    regs[iova::smrOffset(slot)] = 0x80000000u | stream;
    regs[iova::s2crOffset(slot)] = bank;
    regs[(pages + bank) * 4096 + iova::kCbSctlr] = iova::kSctlrM;
  }
};

static void testWindow()
{
  // The table and its translations.
  std::vector<uint64_t> table(2 * 512, 0);
  iova::Window w;
  const uint64_t tablePa = 0x8c0100000ull, arenaPa = 0x8c2000000ull;
  CHECK(iova::buildWindow(w, table.data(), table.data() + 512, tablePa, arenaPa, 24u << 20, kIovaWindowBase));
  CHECK(w.blocks == 12 && w.bytes == (24u << 20));
  CHECK((table[0] & 3) == iova::kDescTable && (table[0] & iova::kOutMask) == tablePa + 4096);
  for (uint32_t block = 0; block < w.blocks; ++block) {
    const uint64_t iova = kIovaWindowBase + uint64_t(block) * kIovaBlockBytes;
    CHECK(iova::translate(w, iova) == arenaPa + uint64_t(block) * kIovaBlockBytes);
    CHECK(iova::translate(w, iova + 0x1000) == arenaPa + uint64_t(block) * kIovaBlockBytes + 0x1000);
    CHECK(iova::busOf(w, uint64_t(block) * kIovaBlockBytes) == iova);
  }
  CHECK(iova::translate(w, kIovaWindowBase - 0x1000) == 0);                    // just below the window
  CHECK(iova::translate(w, kIovaWindowBase + w.bytes) == 0);                   // just above it
  CHECK(iova::translate(w, 0x40000000) == 0);                                  // another gigabyte: nothing is mapped
  CHECK(iova::busOf(w, w.bytes) == 0);
  CHECK(iova::blockDesc(arenaPa) == (arenaPa | iova::kLeafRw | iova::kDescBlock));
  // What the builder refuses: a window or a range that is not 2 MiB aligned, a size that is not whole blocks, a window that would
  // leave the first gigabyte, and a table that is not page aligned.
  iova::Window bad;
  CHECK(!iova::buildWindow(bad, table.data(), table.data() + 512, tablePa, arenaPa + 0x1000, 24u << 20, kIovaWindowBase));
  CHECK(!iova::buildWindow(bad, table.data(), table.data() + 512, tablePa, arenaPa, (24u << 20) + 4096, kIovaWindowBase));
  CHECK(!iova::buildWindow(bad, table.data(), table.data() + 512, tablePa, arenaPa, 24u << 20, 0x40000000));
  CHECK(!iova::buildWindow(bad, table.data(), table.data() + 512, tablePa + 1, arenaPa, 24u << 20, kIovaWindowBase));
  CHECK(!iova::buildWindow(bad, nullptr, nullptr, tablePa, arenaPa, 24u << 20, kIovaWindowBase));

  // The plan: a free group and the highest free bank below the top one.
  SmmuModel model;
  model.occupy(0, 0x100, 10);     // somebody else's stream and bank
  auto rd = [&](uint32_t off) { return model.rd(off); };
  iova::Plan plan = iova::plan(kStreamEndpoint, kSmmuBytes, rd);
  CHECK(!plan.error && plan.slot == 1 && plan.bank == model.banks - 2 && !plan.reuse);
  CHECK(plan.pasize == 2 && plan.cb == (model.pages + plan.bank) * 4096);
  // The bypass entry level 3 left for the same stream is redirected in place, and its bank counts as free.
  SmmuModel reuse;
  reuse.regs[iova::smrOffset(3)] = 0x80000000u | kStreamEndpoint;
  reuse.regs[iova::s2crOffset(3)] = 1u << 16;   // bypass
  auto rd2 = [&](uint32_t off) { return reuse.rd(off); };
  iova::Plan second = iova::plan(kStreamEndpoint, kSmmuBytes, rd2);
  CHECK(!second.error && second.reuse && second.slot == 3);
  // Refusals.
  SmmuModel masked;
  masked.regs[iova::smrOffset(2)] = 0x80000000u | 0xf0000u | kStreamEndpoint;   // a masked group covering the stream
  auto rd3 = [&](uint32_t off) { return masked.rd(off); };
  CHECK(iova::plan(kStreamEndpoint, kSmmuBytes, rd3).error != nullptr);
  SmmuModel small;
  small.regs[iova::kIdr2] = iova::kIdr2Ptfs4k;   // a 32-bit output size
  auto rd4 = [&](uint32_t off) { return small.rd(off); };
  CHECK(iova::plan(kStreamEndpoint, kSmmuBytes, rd4).error != nullptr);
  SmmuModel noStage1;
  noStage1.regs[iova::kIdr0] = 128;
  auto rd5 = [&](uint32_t off) { return noStage1.rd(off); };
  CHECK(iova::plan(kStreamEndpoint, kSmmuBytes, rd5).error != nullptr);
  SmmuModel full;
  for (unsigned i = 0; i < full.groups; ++i) full.occupy(i, uint16_t(0x200 + i), i % 80);
  auto rd6 = [&](uint32_t off) { return full.rd(off); };
  CHECK(iova::plan(kStreamEndpoint, kSmmuBytes, rd6).error != nullptr);

  // The apply: the bank is programmed, then the stream is switched to it, and the stream-match entry is written last.
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  auto wr = [&](uint32_t off, uint32_t value) {
    writes.push_back({off, value});
    model.wr(off, value);
  };
  auto never = [] { return false; };
  CHECK(iova::apply(plan, kStreamEndpoint, tablePa, rd, wr, never) == nullptr);
  CHECK(model.rd(iova::s2crOffset(plan.slot)) == plan.bank);
  CHECK(model.rd(iova::smrOffset(plan.slot)) == (0x80000000u | kStreamEndpoint));
  CHECK(model.rd(plan.cb + iova::kCbTtbr0Lo) == uint32_t(tablePa));
  CHECK(model.rd(plan.cb + iova::kCbTtbr0Hi) == (uint32_t(tablePa >> 32) | (plan.bank & 0xffu) << 16));   // ASID = the bank
  CHECK(model.rd(plan.cb + iova::kCbTcr) == iova::kTcrWindow);
  CHECK(model.rd(plan.cb + iova::kCbTcr2) == iova::tcr2Window(plan.pasize));
  CHECK(model.rd(plan.cb + iova::kCbMair0) == iova::kMair0Nc);
  CHECK(model.rd(iova::cba2rOffset(plan.bank)) == 1 && model.rd(iova::cbarOffset(plan.bank)) == iova::kCbarStage1);
  CHECK((model.rd(plan.cb + iova::kCbSctlr) & iova::kSctlrM) != 0);
  unsigned sctlrFirst = 0, s2cr = 0, smr = 0;
  for (unsigned i = 0; i < writes.size(); ++i) {
    if (writes[i].first == plan.cb + iova::kCbSctlr && writes[i].second == 0) sctlrFirst = i + 1;
    if (writes[i].first == iova::s2crOffset(plan.slot)) s2cr = i + 1;
    if (writes[i].first == iova::smrOffset(plan.slot)) smr = i + 1;
  }
  CHECK(sctlrFirst == 1 && s2cr && smr && s2cr < smr);   // translation off first, the match last
  // A table that is not page aligned, and one above the output size, are refused before anything is written.
  const size_t before = writes.size();
  CHECK(iova::apply(plan, kStreamEndpoint, tablePa + 1, rd, wr, never) != nullptr);
  CHECK(iova::apply(plan, kStreamEndpoint, 0x10000000000ull, rd, wr, never) != nullptr);
  CHECK(writes.size() == before);
  // piano's SMMU: ASIDPNE reads 0 (wifi-l5ib, 2026-11-12); the window is still taken, the other bits must stick.
  for (uint32_t dropped : {iova::kSctlrAsidpne, iova::kSctlrM}) {
    SmmuModel piano;
    auto rdP = [&](uint32_t off) { return piano.rd(off); };
    iova::Plan p2 = iova::plan(kStreamEndpoint, kSmmuBytes, rdP);
    CHECK(!p2.error);
    auto wrP = [&](uint32_t off, uint32_t value) { piano.wr(off, off == p2.cb + iova::kCbSctlr ? value & ~dropped : value); };
    const char *failure = iova::apply(p2, kStreamEndpoint, tablePa, rdP, wrP, never);
    CHECK(dropped == iova::kSctlrAsidpne ? failure == nullptr : failure != nullptr);   // M not sticking is refused
  }
  // A context TLB that never finishes is a timeout, not a hang.
  SmmuModel stuck;
  auto rdStuck = [&](uint32_t off) {
    if (off % 4096 == iova::kCbTlbStatus) return 1u;
    return stuck.rd(off);
  };
  auto wrStuck = [&](uint32_t off, uint32_t value) { stuck.wr(off, value); };
  iova::Plan stuckPlan = iova::plan(kStreamEndpoint, kSmmuBytes, rdStuck);
  CHECK(!stuckPlan.error);
  CHECK(iova::apply(stuckPlan, kStreamEndpoint, tablePa, rdStuck, wrStuck, [] { return true; }) != nullptr);
}

// =========================================================================================== MHI after mission mode
struct RegLog {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  static uint32_t rd(void *ctx, uint32_t reg)
  {
    auto &m = static_cast<RegLog *>(ctx)->regs;
    auto it = m.find(reg);
    return it == m.end() ? 0 : it->second;
  }
  static void wr(void *ctx, uint32_t reg, uint32_t value)
  {
    RegLog &l = *static_cast<RegLog *>(ctx);
    l.regs[reg] = value;
    l.writes.push_back({reg, value});
  }
  MhiIo io() { return MhiIo{this, &RegLog::rd, &RegLog::wr}; }
};

static void testAfterMission()
{
  RegLog l;
  l.regs[kMhiChdbOff] = 0x300;
  CHECK(mhiDevWake(l.io(), 0x1000, true));
  const uint32_t wake = 0x300 + 8 * kMhiDevWakeChannel;
  CHECK(l.writes.size() == 2 && l.writes[0].first == wake + 4 && l.writes[0].second == 0 && l.writes[1].first == wake &&
        l.writes[1].second == 1);
  CHECK(mhiDevWake(l.io(), 0x1000, false) && l.regs[wake] == 0);
  CHECK(!mhiDevWake(l.io(), 0x6f8, true));   // the doorbell would lie past the register space: nothing written
  CHECK(l.writes.size() == 4);
  l.regs[kMhiChdbOff] = 0x302;
  CHECK(!mhiDevWake(l.io(), 0x1000, true));

  // The host's state: the field set, the reset bit and the other bits as they were.
  l.regs[kMhiCtrl] = 0x0000020a;   // M0, reset bit set, bit 3 something else
  mhiSetState(l.io(), kStateM2);
  CHECK(l.regs[kMhiCtrl] == (0x00000008u | kStateM2 << kMhiStateShift));

  // Health.
  const uint32_t bhi = 0x100;
  l.regs[kMhiStatus] = kStateM0 << kMhiStateShift | kMhiStatusReady;
  l.regs[bhi + kBhiExecEnv] = kEeAmss;
  MhiHealth h = mhiHealth(l.io(), bhi);
  CHECK(!h.dead() && !h.gone && h.state == kStateM0 && h.ee == kEeAmss);
  l.regs[kMhiStatus] |= kMhiStatusSysErr;
  CHECK(mhiHealth(l.io(), bhi).sysErr && mhiHealth(l.io(), bhi).dead());
  l.regs[kMhiStatus] = kStateSysErr << kMhiStateShift;
  CHECK(mhiHealth(l.io(), bhi).sysErr);
  l.regs[kMhiStatus] = kStateM0 << kMhiStateShift;
  l.regs[bhi + kBhiExecEnv] = kEeRddm;
  CHECK(mhiHealth(l.io(), bhi).rddm && mhiHealth(l.io(), bhi).dead());
  l.regs[kMhiStatus] = 0xffffffffu;
  h = mhiHealth(l.io(), bhi);
  CHECK(h.gone && !h.dead());
}

// ========================================================================================================== the crash dump
// A dump as the firmware writes it: the header (version, size or size, version), the entries, then the files back to back.
struct DumpFile { const char *name, *description; uint64_t size; std::string content; };
static std::vector<uint8_t> makeDump(const std::vector<DumpFile> &files, bool sizeFirst = false)
{
  const uint32_t header = kRddmHeaderWords + uint32_t(files.size()) * kRddmEntryBytes;
  std::vector<uint8_t> d(header, 0);
  put32(d.data(), sizeFirst ? header : 1);
  put32(d.data() + 4, sizeFirst ? 1 : header);
  for (size_t i = 0; i < files.size(); ++i) {
    uint8_t *e = d.data() + kRddmHeaderWords + i * kRddmEntryBytes;
    put64(e, 0x86000000ull + i * 0x100000);
    put64(e + 8, 0x4000000ull + i);
    put64(e + 16, files[i].size);
    std::strncpy(reinterpret_cast<char *>(e + 24), files[i].description, kRddmNameBytes);
    std::strncpy(reinterpret_cast<char *>(e + 44), files[i].name, kRddmNameBytes);
  }
  for (const DumpFile &f : files) {
    std::string body = f.content;
    body.resize(size_t(f.size), 'x');
    d.insert(d.end(), body.begin(), body.end());
  }
  return d;
}

struct DumpHost {
  std::vector<uint8_t> memory;
  Arena arena;
  Rddm r;
  DumpHost() : memory(8u << 20, 0)
  {
    arenaInit(arena, memory.data(), kBusBase, uint32_t(memory.size()));
    CHECK(rddmPlan(r, arena, kRddmBytes));
  }
  // The device's side: the dump written across the segments the table names.
  void write(const std::vector<uint8_t> &dump)
  {
    uint64_t at = 0;
    for (uint32_t s = 0; s < r.segments && at < dump.size(); ++s) {
      const uint64_t address = get64(r.table + 16 * s), size = get64(r.table + 16 * s + 8);
      uint8_t *to = memory.data() + (address - kBusBase);
      const uint64_t n = dump.size() - at < size ? dump.size() - at : size;
      std::memcpy(to, dump.data() + at, size_t(n));
      at += n;
    }
  }
  void decode(RddmReport &out, uint32_t serial = 0)
  {
    std::vector<uint8_t> scratch(kRddmScratchBytes);
    rddmDecode(r, scratch.data(), [](uint8_t *to, const uint8_t *from, uint32_t n) { std::memcpy(to, from, n); }, serial, out);
  }
};

static void testRddm()
{
  {   // the plan
    DumpHost h;
    CHECK(h.r.segments == 9 && h.r.segBytes == kRddmSegBytes && h.r.tableBytes == 9 * 16);
    for (uint32_t i = 0; i < h.r.segments; ++i) {
      CHECK(h.r.segBus[i] % kRddmSegBytes == 0);
      CHECK(get64(h.r.table + 16 * i) == h.r.segBus[i] && get64(h.r.table + 16 * i + 8) == kRddmSegBytes);
    }
    CHECK(h.r.tableBus % 4096 == 0 && h.r.tableBus > h.r.segBus[8]);
    Rddm none;
    Arena small;
    std::vector<uint8_t> two(2u << 20);
    arenaInit(small, two.data(), kBusBase, uint32_t(two.size()));
    CHECK(!rddmPlan(none, small, kRddmBytes) && none.segments == 0);   // 9 segments do not fit 2 MiB
    CHECK(!rddmPlan(none, small, 0));
    CHECK(!rddmPlan(none, small, 17 * kRddmSegBytes));                // more segments than the table plans for
  }
  {   // the arm and the receive status
    DumpHost h;
    RegLog l;
    const uint32_t bhie = 0x224;
    l.regs[bhie + kBhieRxVecDb] = 0xc0000000u;   // the doorbell's other bits stay as they are
    rddmArm(h.r, l.io(), bhie, 0x40001234u);
    CHECK(h.r.seq == 0x1234 && h.r.arms == 1);
    CHECK(l.writes.size() == 4);
    CHECK(l.writes[0].first == bhie + kBhieRxVecAddrHi && l.writes[0].second == uint32_t(h.r.tableBus >> 32));
    CHECK(l.writes[1].first == bhie + kBhieRxVecAddrLo && l.writes[1].second == uint32_t(h.r.tableBus));
    CHECK(l.writes[2].first == bhie + kBhieRxVecSize && l.writes[2].second == 9 * 16);
    CHECK(l.writes[3].first == bhie + kBhieRxVecDb && l.writes[3].second == (0xc0000000u | 0x1234u));
    rddmArm(h.r, l.io(), bhie, 0);
    CHECK(h.r.seq == 1);   // a session is never 0
    CHECK(rddmPoll(h.r, l.io(), bhie) == kRddmWaiting);
    l.regs[bhie + kBhieRxVecStatus] = kMhiXferSuccess << 30 | 1;
    CHECK(rddmPoll(h.r, l.io(), bhie) == kRddmComplete && rddmSessionMatches(h.r));
    l.regs[bhie + kBhieRxVecStatus] = kMhiXferSuccess << 30 | 5;
    CHECK(rddmPoll(h.r, l.io(), bhie) == kRddmComplete && !rddmSessionMatches(h.r));
    l.regs[bhie + kBhieRxVecStatus] = kMhiXferError << 30 | 1;
    CHECK(rddmPoll(h.r, l.io(), bhie) == kRddmError);
    l.regs[bhie + kBhieRxVecStatus] = 0xffffffffu;
    CHECK(rddmPoll(h.r, l.io(), bhie) == kRddmWaiting);
  }
  {   // a dump with a failure reason that crosses into the second segment
    DumpHost h;
    const uint32_t serial = 0x1a2b3c4du;
    const std::string reason = std::string("wlan_fw.c:1234 Assertion pmu_ready failed\r\n  (serial 1a2b3c4d)\x01") +
                               std::string(1, '\0') + "trailing bytes nobody prints";
    const std::vector<DumpFile> files = {
      {"Q6-DUMP.bin", "Q6 memory", 600000, ""},
      {"Q6-SFR.bin", "Failure reason", 96, reason},
      {"Q6-REGS.bin", "Registers", 4096, ""},
    };
    h.write(makeDump(files));
    RddmReport rep;
    h.decode(rep, serial);
    CHECK(rep.header && !rep.sizeFirst && rep.version == 1 && rep.headerBytes == 8 + 3 * 64 && rep.entries == 3 && rep.files == 3);
    CHECK(std::strcmp(rep.file[0].name, "Q6-DUMP.bin") == 0 && std::strcmp(rep.file[0].description, "Q6 memory") == 0);
    CHECK(rep.file[0].offset == rep.headerBytes && rep.file[1].offset == rep.headerBytes + 600000 &&
          rep.file[2].offset == rep.headerBytes + 600096);
    CHECK(rep.file[0].inDump && rep.file[1].inDump && rep.file[2].inDump);
    CHECK(rep.file[0].base == 0x86000000ull && rep.file[1].phys == 0x4000001ull);
    CHECK(rep.reasonFile == 1);
    CHECK(std::strcmp(rep.reason, "wlan_fw.c:1234 Assertion pmu_ready failed (serial --------).") == 0);
    CHECK(rep.strings == 0);   // a reason was found: nothing else is looked for
    CHECK(rep.segmentsWithData == 2 && rep.dataEnd == rep.headerBytes + 600000 + 96 + 4096);
  }
  {   // the size before the version; a reason longer than the log takes; the decimal form of the serial
    DumpHost h;
    const uint32_t serial = 987654321u;
    const std::string reason = "err_fatal: id 987654321 " + std::string(400, 'z');
    h.write(makeDump({{"q6-sfr.BIN", "sfr", 2048, reason}}, true));
    RddmReport rep;
    h.decode(rep, serial);
    CHECK(rep.header && rep.sizeFirst && rep.headerBytes == 72 && rep.version == 1);
    CHECK(rep.reasonFile == 0 && std::strlen(rep.reason) == kRddmReasonBytes);
    CHECK(std::strncmp(rep.reason, "err_fatal: id --------- zzz", 27) == 0);
  }
  {   // no header: the first segment's error-like strings, the strong ones first, format strings never
    DumpHost h;
    std::vector<uint8_t> dump(64 * 1024, 0);
    auto at = [&](size_t off, const std::string &s) { std::memcpy(dump.data() + off, s.data(), s.size()); };
    put32(dump.data(), 0xdeadbeefu);
    put32(dump.data() + 4, 0x12345678u);
    at(0x100, "an ordinary sentence with nothing in it");
    at(0x200, "cnss: timed out waiting for the host");                // weak
    at(0x300, "%s: failed to start %d, ERR_FATAL at %p");              // a format string: never
    at(0x400, "ERR_FATAL wdog bite: core 0 pc 0x0001f00d");            // strong
    at(0x500, "short err");                                            // too short
    at(0x600, "phy_init.c:77 assertion " + std::string(300, 'q'));    // strong, long: cut at 120
    at(0x900, "timeout one");                                          // too short
    at(0xa00, "a fault was seen on the bus here");                     // weak
    at(0xb00, "another fail in the radio calibration");                // weak
    at(0xc00, "and an error that is the fourth weak one");             // weak, one too many to keep
    h.write(dump);
    RddmReport rep;
    h.decode(rep);
    CHECK(!rep.header && rep.why != nullptr && rep.word0 == 0xdeadbeefu && rep.word1 == 0x12345678u);
    CHECK(rep.reasonFile == -1 && rep.reason[0] == 0);
    CHECK(rep.strings == kRddmMaxStrings && rep.candidates == 6);
    CHECK(std::strcmp(rep.string[0].text, "ERR_FATAL wdog bite: core 0 pc 0x0001f00d") == 0 && rep.string[0].at == 0x400);
    CHECK(std::strlen(rep.string[1].text) == kRddmStringBytes && rep.string[1].at == 0x600);
    CHECK(std::strcmp(rep.string[2].text, "cnss: timed out waiting for the host") == 0);
    CHECK(std::strcmp(rep.string[3].text, "a fault was seen on the bus here") == 0);
    for (unsigned i = 0; i < rep.strings; ++i) CHECK(std::strchr(rep.string[i].text, '%') == nullptr);
  }
  {   // an all-zero dump: nothing decoded, nothing printed
    DumpHost h;
    RddmReport rep;
    h.decode(rep);
    CHECK(!rep.header && rep.dataEnd == 0 && rep.segmentsWithData == 0 && rep.strings == 0 && rep.why != nullptr);
  }
  {   // a header whose entries carry no names is not a header; a file larger than the dump is flagged, the walk goes on
    DumpHost h;
    std::vector<uint8_t> dump = makeDump({{"", "", 64, "nothing"}});
    h.write(dump);
    RddmReport rep;
    h.decode(rep);
    CHECK(!rep.header && rep.files == 0 && rep.why != nullptr);
    DumpHost g;
    g.write(makeDump({{"Q6-HUGE.bin", "memory", 64ull << 20, ""}, {"Q6-SFR.bin", "reason", 32, "never reached"}}));
    // the second file's offset is past the dump: no reason is read
    RddmReport rep2;
    g.decode(rep2);
    CHECK(rep2.header && rep2.files == 2 && !rep2.file[0].inDump && !rep2.file[1].inDump && rep2.reasonFile == -1);
  }
  {   // the read across segments
    DumpHost h;
    std::vector<uint8_t> dump(kRddmSegBytes + 16, 0);
    for (size_t i = 0; i < dump.size(); ++i) dump[i] = uint8_t(i * 7 + 1);
    h.write(dump);
    uint8_t buf[32] = {};
    auto copy = [](uint8_t *to, const uint8_t *from, uint32_t n) { std::memcpy(to, from, n); };
    CHECK(rddmRead(h.r, kRddmSegBytes - 8, buf, 16, copy) == 16);
    for (unsigned i = 0; i < 16; ++i) CHECK(buf[i] == uint8_t((kRddmSegBytes - 8 + i) * 7 + 1));
    CHECK(rddmRead(h.r, uint64_t(h.r.segments) * kRddmSegBytes - 4, buf, 16, copy) == 4);   // clipped at the end
    CHECK(rddmRead(h.r, uint64_t(h.r.segments) * kRddmSegBytes, buf, 16, copy) == 0);
  }
  {   // the mask: hex in either case, never a partial match, and nothing for a serial of 0
    char text[] = "id 1A2B3C4D and 1a2b3c4d and 1a2b3c4 end";
    rddmMask(text, 0x1a2b3c4du);
    CHECK(std::strcmp(text, "id -------- and -------- and 1a2b3c4 end") == 0);
    char untouched[] = "00000000";
    rddmMask(untouched, 0);
    CHECK(std::strcmp(untouched, "00000000") == 0);
  }
}

int main()
{
  testCodec();
  testTransport();
  testRings();
  testAfterMission();
  testRddm();
  testRouter();
  testClient();
  testClientGate();
  testClientFaults();
  testFlowControl();
  testShadowList();
  testWindow();
  std::printf(failures ? "qmi_host: %d check(s) failed\n" : "qmi_host: all passed\n", failures);
  return failures ? 1 : 0;
}
