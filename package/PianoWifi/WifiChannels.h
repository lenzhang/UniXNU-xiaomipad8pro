// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 5, the transport: the chip's two IPC router channels over MHI, as a portable piece the owner steps (host tests:
// tests/qmi_host.cpp against a device model). New code for this repository, on level 4's arena and contexts (WifiMhi.h); the
// register, context and ring-element facts are WifiHw.h's, read from the MHI specification's names as the mainline and vendor hosts
// use them (behaviour only, nothing copied). Design re-expressed from UniXNU-sheng's and PaganiWifi's MHI boot (BSD-2-Clause-Patent,
// UPSTREAM), which stop at mission mode: the channels, their rings and the event walk are this driver's own.
//
// What it does:
//   0. before the boot (level 4, when level 5 follows): the rings and buffers out of the arena; the two channel contexts (20 to the
//      device, 21 back) carry only their fixed fields, which level 4's context plan wrote (state disabled, burst mode disabled,
//      the type, the event ring), as the MHI host's context initialisation leaves them;
//   1. once the firmware is in mission mode, for each channel in turn (20, then 21, as the MHI host prepares a device's two
//      channels): its ring into its context and the state enabled, then
//   2. a "start channel" command on the command ring, and the chip answers with a command completion event;
//   3. every element of the inbound ring but one gets an 8 KiB buffer of the arena, and the chip is told (one doorbell);
//   4. a packet to the chip is written straight into one of our four outbound buffers (no copy of the 6 KiB downloads), given one
//      element of the outbound ring, and one doorbell;
//   5. polling walks the two event rings: a transfer completion on 21 is a received packet (the chip wrote its length), one on 20
//      frees that outbound buffer, and state or execution-environment events are reported to the owner (a system error or a crash
//      dump environment ends level 5).
// Nothing here maps memory, allocates or sleeps: the arena is the owner's, and every wait is the owner's bounded poll.
#pragma once
#include <stdint.h>
#include "WifiHw.h"
#include "WifiMhi.h"

namespace pianowifi {

constexpr uint32_t kIpcrRxBuffers = kIpcrElements - 1;   // a transfer ring is never filled completely: one element stays free

struct ChanRing {
  uint8_t *va = nullptr, *ctx = nullptr;
  uint64_t bus = 0;
  uint32_t chan = 0, elements = 0, wp = 0;   // wp: the element we fill next (the chip reads up to it)
};

struct IpcrBuffer {
  uint8_t *va = nullptr;
  uint64_t bus = 0;
  uint32_t bytes = 0, length = 0;
  bool busy = false;
};

enum IpcrEvent : uint32_t { kIpcrNothing = 0, kIpcrPacket, kIpcrTxDone, kIpcrCommand, kIpcrState, kIpcrEe };

struct Ipcr {
  ChanRing out, in;
  ChanRing cmd;                                  // the primary command ring level 4 planned
  IpcrBuffer rx[kIpcrRxBuffers];
  IpcrBuffer tx[kIpcrOutBuffers];
  uint32_t chdb = 0, erdb = 0, reglen = 0;
  bool started[2] = {false, false};               // the two channels' start commands completed
  uint64_t cmdPending = 0;                        // bus address of the command element waiting for its completion
  uint32_t cmdChannel = 0;
  // counters and the last things seen, for the log
  uint32_t sent = 0, received = 0, txDone = 0, commands = 0, commandErrors = 0, states = 0, ees = 0, dropped = 0, refused = 0;
  uint32_t lastState = 0, lastEe = 0, lastCode = 0;
  const char *error = nullptr;
};

namespace ipcrdetail {
inline void put32(uint8_t *at, uint32_t v)
{
  for (unsigned i = 0; i < 4; ++i) at[i] = uint8_t(v >> (8 * i));
}
inline void put64(uint8_t *at, uint64_t v)
{
  put32(at, uint32_t(v));
  put32(at + 4, uint32_t(v >> 32));
}
inline uint32_t get32(const uint8_t *at)
{
  uint32_t v = 0;
  for (unsigned i = 0; i < 4; ++i) v |= uint32_t(at[i]) << (8 * i);
  return v;
}
inline uint64_t get64(const uint8_t *at)
{
  return uint64_t(get32(at)) | uint64_t(get32(at + 4)) << 32;
}
// A doorbell is a 64-bit address written as two words, the high half first (the chip latches on the low half).
inline void doorbell(const MhiIo &io, uint32_t reg, uint64_t value)
{
  io.write(io.ctx, reg + 4, uint32_t(value >> 32));
  io.write(io.ctx, reg, uint32_t(value));
}
inline void element(uint8_t *at, uint64_t ptr, uint32_t word0, uint32_t word1)
{
  put64(at, ptr);
  put32(at + 8, word0);
  put32(at + 12, word1);
}
}  // namespace ipcrdetail

// The rings and buffers out of the arena (each ring aligned to its own 1 KiB length by the arena's rule), and the two channel
// contexts with their fixed fields only: state disabled, burst mode disabled, the type and the event ring, no ring yet. The command
// ring is the one level 4 planned (its address comes from the contexts). False when the arena has no room for all of it.
inline bool ipcrPlan(Ipcr &p, Arena &arena, MhiContexts &contexts)
{
  using namespace ipcrdetail;
  if (!contexts.va || !arena.va) return false;
  const uint32_t ringBytes = kIpcrElements * kMhiElement;
  struct Setup { ChanRing *ring; uint32_t chan, type; };
  const Setup setup[2] = {{&p.out, kChanIpcrOut, kChanTypeOutbound}, {&p.in, kChanIpcrIn, kChanTypeInbound}};
  for (const Setup &s : setup) {
    uint64_t bus = 0;
    uint8_t *va = arenaAlloc(arena, ringBytes, &bus);
    if (!va) return false;
    s.ring->va = va;
    s.ring->bus = bus;
    s.ring->chan = s.chan;
    s.ring->elements = kIpcrElements;
    s.ring->wp = 0;
    s.ring->ctx = mhiChannelCtx(contexts, s.chan);
    put32(s.ring->ctx + kChanCfg, kChanStateDisabled | kChanBurstDisable << 8);
    put32(s.ring->ctx + kChanType, s.type);
    put32(s.ring->ctx + kChanErIndex, kIpcrEventRing);
  }
  for (IpcrBuffer &b : p.rx) {
    b.va = arenaAlloc(arena, kIpcrBufferBytes, &b.bus);
    if (!b.va) return false;
    b.bytes = kIpcrBufferBytes;
  }
  for (IpcrBuffer &b : p.tx) {
    b.va = arenaAlloc(arena, kIpcrBufferBytes, &b.bus);
    if (!b.va) return false;
    b.bytes = kIpcrBufferBytes;
  }
  // The primary command ring is level 4's: its context on the page mhiContextsPlan gave it, its elements where it put them.
  p.cmd.ctx = mhiCommandCtx(contexts);
  p.cmd.va = contexts.va + uint32_t(contexts.commandBus - contexts.bus);
  p.cmd.bus = contexts.commandBus;
  p.cmd.elements = kMhiCommandElements;
  p.cmd.wp = 0;
  return true;
}

// A channel about to be started: its ring into its context (base, length, both pointers at the base) and the state enabled, the
// burst mode and the other fixed fields kept, as the MHI host prepares a channel right before its start command.
inline bool ipcrPrepare(ChanRing &r)
{
  using namespace ipcrdetail;
  if (!r.ctx || !r.va) return false;
  r.wp = 0;
  put64(r.ctx + kChanRBase, r.bus);
  put64(r.ctx + kChanRLen, uint64_t(r.elements) * kMhiElement);
  put64(r.ctx + kChanRp, r.bus);
  put64(r.ctx + kChanWp, r.bus);
  put32(r.ctx + kChanCfg, (get32(r.ctx + kChanCfg) & ~0xffu) | kChanStateEnabled);
  return true;
}

// The doorbell arrays, checked against the register space the owner mapped. False (and `error` set) when the chip reports them
// outside it, which would mean the register layout is not the one level 4 read.
inline bool ipcrOpen(Ipcr &p, const MhiIo &io, uint32_t reglen)
{
  p.reglen = reglen;
  p.chdb = io.read(io.ctx, kMhiChdbOff);
  p.erdb = io.read(io.ctx, kMhiErdbOff);
  const bool good = p.chdb >= 0x100 && !(p.chdb & 3) && p.chdb + 8 * (kChanIpcrIn + 1) <= reglen && p.erdb >= 0x100 &&
                    !(p.erdb & 3) && p.erdb + 8 * kMhiEventRings <= reglen;
  if (!good) {
    p.error = "the channel or event doorbells are outside the register space";
    return false;
  }
  return true;
}

// One command on the primary command ring (one at a time: level 5 only ever starts two channels).
inline bool ipcrCommand(Ipcr &p, const MhiIo &io, uint32_t channel, uint32_t command)
{
  using namespace ipcrdetail;
  if (p.cmdPending) {
    p.error = "a command is already in flight";
    return false;
  }
  const uint64_t at = p.cmd.bus + uint64_t(p.cmd.wp) * kMhiElement;
  element(p.cmd.va + p.cmd.wp * kMhiElement, 0, 0, channel << 24 | command << 16);
  p.cmd.wp = (p.cmd.wp + 1) % p.cmd.elements;
  const uint64_t wp = p.cmd.bus + uint64_t(p.cmd.wp) * kMhiElement;
  put64(p.cmd.ctx + kChanWp, wp);
  doorbell(io, kMhiCrdbLo, wp);
  p.cmdPending = at;
  p.cmdChannel = channel;
  return true;
}

// One of the two channels started: prepared (its ring and the enabled state), then its start command. False when nothing was
// queued (`error` says why).
inline bool ipcrStart(Ipcr &p, const MhiIo &io, uint32_t channel)
{
  ChanRing *r = channel == kChanIpcrOut ? &p.out : channel == kChanIpcrIn ? &p.in : nullptr;
  if (!r || !ipcrPrepare(*r)) {
    p.error = "not one of the two planned channels";
    return false;
  }
  if (p.cmdPending) {
    p.error = "a command is already in flight";
    return false;
  }
  return ipcrCommand(p, io, channel, kTreCmdStart);
}

// What the MHI host does when the device reports M0 in mission mode: every event ring's doorbell rung again with the write pointer
// its context holds, the command ring's when a command waits, and each running channel's. Returns the number of doorbells.
inline unsigned ipcrRingAll(Ipcr &p, MhiContexts &contexts, const MhiIo &io)
{
  using namespace ipcrdetail;
  if (!contexts.va || !p.erdb || !p.chdb) return 0;
  unsigned rung = 0;
  for (unsigned r = 0; r < kMhiEventRings; ++r, ++rung) doorbell(io, p.erdb + 8 * r, get64(mhiEventCtx(contexts, r) + 36));
  if (p.cmdPending) {
    doorbell(io, kMhiCrdbLo, get64(p.cmd.ctx + kChanWp));
    ++rung;
  }
  if (p.started[0]) {
    doorbell(io, p.chdb + 8 * kChanIpcrOut, get64(p.out.ctx + kChanWp));
    ++rung;
  }
  if (p.started[1]) {
    doorbell(io, p.chdb + 8 * kChanIpcrIn, get64(p.in.ctx + kChanWp));
    ++rung;
  }
  return rung;
}

// Every inbound element but one gets a buffer, then one doorbell. Returns how many were posted.
inline unsigned ipcrPostRx(Ipcr &p, const MhiIo &io)
{
  using namespace ipcrdetail;
  unsigned posted = 0;
  for (IpcrBuffer &b : p.rx) {
    if (b.busy || !b.va) continue;
    const uint32_t slot = p.in.wp;
    element(p.in.va + slot * kMhiElement, b.bus, b.bytes & 0xffffu, kTreTypeTransfer << 16 | kTreIeot);
    b.busy = true;
    b.length = 0;
    p.in.wp = (slot + 1) % p.in.elements;
    ++posted;
  }
  if (!posted) return 0;
  const uint64_t wp = p.in.bus + uint64_t(p.in.wp) * kMhiElement;
  put64(p.in.ctx + kChanWp, wp);
  doorbell(io, p.chdb + 8 * kChanIpcrIn, wp);
  return posted;
}

// A free outbound buffer to build a packet in (its bus address is the chip's); -1 when all four are in flight. The buffer is
// held from here until the chip's completion event frees it, so that nothing can hand out the same buffer twice — the owner
// builds a message in it over several calls (a download reads a file into it) while the router may need one of its own.
inline int ipcrClaimTx(Ipcr &p)
{
  for (unsigned i = 0; i < kIpcrOutBuffers; ++i)
    if (!p.tx[i].busy && p.tx[i].va) {
      p.tx[i].busy = true;
      p.tx[i].length = 0;
      return int(i);
    }
  ++p.refused;
  return -1;
}

// A claim the owner decided not to use.
inline void ipcrReleaseTx(Ipcr &p, unsigned index)
{
  if (index < kIpcrOutBuffers) p.tx[index].busy = false;
}

// The claimed buffer, `length` bytes of it, on the outbound ring; one doorbell. False when it was not claimed or the ring is
// full (the claim is then given back).
inline bool ipcrSubmitTx(Ipcr &p, const MhiIo &io, unsigned index, uint32_t length)
{
  using namespace ipcrdetail;
  if (index >= kIpcrOutBuffers || !p.tx[index].va || !p.tx[index].busy || !length || length > p.tx[index].bytes) return false;
  const uint32_t slot = p.out.wp, next = (slot + 1) % p.out.elements;
  const uint64_t rp = get64(p.out.ctx + kChanRp);
  if (p.out.bus + uint64_t(next) * kMhiElement == rp) {   // the ring would look empty to the chip again
    ipcrReleaseTx(p, index);
    return false;
  }
  element(p.out.va + slot * kMhiElement, p.tx[index].bus, length & 0xffffu, kTreTypeTransfer << 16 | kTreIeot);
  p.tx[index].length = length;
  p.out.wp = next;
  const uint64_t wp = p.out.bus + uint64_t(next) * kMhiElement;
  put64(p.out.ctx + kChanWp, wp);
  doorbell(io, p.chdb + 8 * kChanIpcrOut, wp);
  ++p.sent;
  return true;
}

// One event ring walked: every element the chip wrote since the last walk is handed to `deliver(kind, chan, code, length, va)`
// (va is the receive buffer for a packet, null otherwise), then the elements are given back to the chip (one doorbell). Returns how
// many events were seen. A received buffer is recycled by the caller's own ipcrPostRx after it has used the packet.
template <class Deliver>
inline unsigned ipcrPollRing(Ipcr &p, MhiContexts &contexts, const MhiIo &io, unsigned ring, Deliver deliver)
{
  using namespace ipcrdetail;
  if (ring >= kMhiEventRings || !contexts.va || !p.erdb) return 0;
  uint8_t *ctx = mhiEventCtx(contexts, ring);
  const uint64_t base = get64(ctx + 12), length = get64(ctx + 20), rp = get64(ctx + 28);
  if (!length || rp < base || rp >= base + length || (rp - base) % kMhiElement) {
    if (rp != base) ++p.dropped;
    return 0;
  }
  unsigned seen = 0;
  uint64_t at = contexts.acked[ring];
  if (at < base || at >= base + length) at = base;
  while (at != rp && seen < kIpcrElements + kMhiCommandElements + 8) {
    const uint8_t *e = contexts.eventVa[ring] + (at - base);
    const uint64_t ptr = get64(e);
    const uint32_t word0 = get32(e + 8), word1 = get32(e + 12);
    const uint32_t type = (word1 >> 16) & 0xffu, chan = (word1 >> 24) & 0xffu;
    const uint32_t code = (word0 >> 24) & 0xffu, len = word0 & 0xffffu;
    switch (type) {
    case kEvTypeTransfer: {
      p.lastCode = code;
      if (chan == kChanIpcrIn) {
        IpcrBuffer *b = nullptr;
        for (IpcrBuffer &candidate : p.rx)
          if (candidate.bus == ptr) b = &candidate;
        if (!b || !b->busy || len > b->bytes) {
          ++p.dropped;
          break;
        }
        b->busy = false;
        b->length = len;
        ++p.received;
        deliver(kIpcrPacket, chan, code, len, b->va);
      } else if (chan == kChanIpcrOut) {
        for (IpcrBuffer &candidate : p.tx)
          if (candidate.bus == ptr && candidate.busy) {
            candidate.busy = false;
            ++p.txDone;
            deliver(kIpcrTxDone, chan, code, len, candidate.va);
          }
      } else {
        ++p.dropped;
      }
      break;
    }
    case kEvTypeCmdCompletion:
      ++p.commands;
      p.lastCode = code;
      if (p.cmdPending && ptr == p.cmdPending) {
        p.cmdPending = 0;
        if (code == kEvCodeSuccess) {
          if (p.cmdChannel == kChanIpcrOut) p.started[0] = true;
          if (p.cmdChannel == kChanIpcrIn) p.started[1] = true;
        } else {
          ++p.commandErrors;
        }
      } else {
        ++p.dropped;
      }
      deliver(kIpcrCommand, p.cmdChannel, code, 0, nullptr);
      break;
    case kEvTypeStateChange:
      ++p.states;
      p.lastState = code;
      deliver(kIpcrState, chan, code, 0, nullptr);
      break;
    case kEvTypeEeChange:
      ++p.ees;
      p.lastEe = code;
      deliver(kIpcrEe, chan, code, 0, nullptr);
      break;
    default:
      ++p.dropped;
      break;
    }
    ++seen;
    at += kMhiElement;
    if (at >= base + length) at = base;
  }
  if (seen) {
    contexts.acked[ring] = at;
    const uint64_t give = at == base ? base + length - kMhiElement : at - kMhiElement;
    put64(ctx + 36, give);
    doorbell(io, p.erdb + 8 * ring, give);
    contexts.acks += 1;
  }
  return seen;
}

// Both rings: the control ring first (a state or environment change outranks a packet), then the data ring the channels report on.
template <class Deliver>
inline unsigned ipcrPoll(Ipcr &p, MhiContexts &contexts, const MhiIo &io, Deliver deliver)
{
  unsigned seen = ipcrPollRing(p, contexts, io, 0, deliver);
  seen += ipcrPollRing(p, contexts, io, kIpcrEventRing, deliver);
  return seen;
}

inline bool ipcrRunning(const Ipcr &p) { return p.started[0] && p.started[1]; }

// The channel states as the contexts hold them, for the log.
inline uint32_t ipcrChanState(const ChanRing &r)
{
  return r.ctx ? (ipcrdetail::get32(r.ctx + kChanCfg) & 0xffu) : kChanStateDisabled;
}

}  // namespace pianowifi
