// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoDpu B1e: the panel's light sleep with the display (docs/drivers/dpu.md §4.5), portable (tests/dpu_panel_host.cpp).
//
// Off (after PianoBacklight turned the backlight off on the same display-power change): DCS display-off (0x28), 20 ms, sleep-in
// (0x10), 100 ms (the panel's stock term sequence and Ubuntu's P81 shutdown timing, K:72cfce4c86e5), then the DPU's timing engines
// stopped, INTF2 (the slave) before INTF1, each until INTF_STATUS bit 0 clears (at most 40 ms, Ubuntu's slave poll, K:c55958fa09dd):
// the DPU stops fetching (3.28 GB/s at 120 Hz) and the panel stops being driven. Nothing else: the DSI controllers, the PHYs, MDSS,
// the CTL (never reset while anything scans), the panel's reset gpio98 and its supplies (vddio L12B; AVDD/AVEE from the KTZ8866's
// bias, ENP/ENN = gpio117/118) all stay as they are (DpuSlices.h's store guard cannot reach TLMM at all).
// On: CTL_INTF_FLUSH + CTL_FLUSH bit 31, then INTF1 and INTF2 enabled back to back (msm's order: flush before the timing engine,
// master first), both counting again in lockstep, then DCS sleep-out (0x11), 120 ms, display-on (0x29), 10 ms; the caller re-applies
// the PCC and PianoBacklight lights the panel.
// The DCS commands go out while the video stream runs (the command engine sends in the vertical blanking; its DMA reads the
// controller's own command FIFO, so no buffer has to be reachable by the DSI's DMA through the SMMU), on both links, one trigger each
// (DSI0 then DSI1, back to back, as msm sends a bonded pair's commands). Per link, from msm's dsi_host.c (the video-mode transfer:
// command mode enabled beside video for the transfer, the DMA's low-power bit as the panel's MIPI_DSI_MODE_LPM asks, the done bit)
// and UniXNU-sm8750-common's PeripheralIO.h (BSD-2-Clause-Patent: the FIFO route on SM8750, its words and its restore):
//   - refuse unless CTRL has enable + video, TEST_PATTERN_GEN_CTRL is 0 (the FIFO free) and TRIG_CTRL's DMA trigger is software
//     (4); wait until STATUS0's command-DMA busy bit (1) is clear (20 ms);
//   - INTR_CTRL = its enables (odd bits) | 1 (the done bit is write-one-to-clear); CTRL |= command mode (bit 2);
//     TEST_PATTERN_GEN_CTRL = 0x30006 (CMD_DMA_TPG_EN, TPG_DMA_FIFO_MODE, pattern 3: the DMA FIFO); the packet's words into the
//     FIFO (+0x17c), an odd count padded with a zero word; CMD_DMA_CTRL: broadcast (31) and master (30) cleared, FROM_FRAME_BUFFER
//     (28) set, LOW_POWER (26) set for LP; the second DMA control word (+0x390) bit 1 cleared; DMA length = the packet's bytes;
//   - TRIG_DMA = 1 on DSI0 and DSI1; when TRIG_CTRL's BLOCK_DMA_WITHIN_FRAME (bit 12) is clear the trigger waits for INTF1's line
//     counter in the front porch first; INTR_CTRL bit 0 on both within 50 ms;
//   - restore: INTR_CTRL enables | 1, TEST_PATTERN_GEN_CTRL = 0, +0x1ec pulsed 1 then 0 (PeripheralIO's FIFO reset; SunUEFI's
//     DSI timing DB mode word, idle here either way), CMD_DMA_CTRL, +0x390, the length and CTRL as found. A link whose DMA did not
//     finish is not restored (the engine still owns the command) and the slice stops for the boot.
// A DCS short write without a parameter: data type 0x05, header word = last-command flag (bit 31) | type << 16 | command.
#pragma once
#include "DpuSlices.h"

namespace pianodpu {
namespace panel {
using namespace slices;

constexpr uint8_t kDcsDisplayOff = 0x28, kDcsSleepIn = 0x10, kDcsSleepOut = 0x11, kDcsDisplayOn = 0x29;
constexpr uint32_t kCtrlEnable = 1u, kCtrlVideo = 1u << 1, kCtrlCommand = 1u << 2;
constexpr uint32_t kStatusDmaBusy = 1u << 1;
constexpr uint32_t kIntDmaDone = 1u, kIntEnables = 0xa222aa02u;   // the odd (enable) bits PeripheralIO keeps; bit 0 = CMD_DMA_DONE
constexpr uint32_t kTpgFifo = 0x30006u;
constexpr uint32_t kDmaBroadcast = 1u << 31, kDmaMaster = 1u << 30, kDmaFromBuffer = 1u << 28, kDmaLowPower = 1u << 26;
constexpr uint32_t kTrigDmaMask = 7u, kTrigDmaSw = 4u, kTrigBlockDmaInFrame = 1u << 12;
constexpr uint32_t kDmaCtrl1Bit = 1u << 1;
constexpr uint32_t kDmaDoneTimeoutUs = 50000, kDmaBusyTimeoutUs = 20000, kEngineStopUs = 40000, kEngineStartUs = 50000;

inline bool dcsAllowed(uint8_t c) { return c == kDcsDisplayOff || c == kDcsSleepIn || c == kDcsSleepOut || c == kDcsDisplayOn; }
struct Packet {
  uint32_t words[2] = {0, 0};
  unsigned count = 0;
  uint32_t bytes = 0;
};
inline Packet dcsShort(uint8_t command)
{
  Packet p;
  p.words[0] = 0x80000000u | (0x05u << 16) | command;
  p.words[1] = 0;   // the odd count's pad word
  p.count = 2;
  p.bytes = 4;
  return p;
}

struct Options {
  bool lowPower = true;   // the panel's MIPI_DSI_MODE_LPM (piano-dpu-panel-hs=1: high speed)
  bool engines = true;    // stop the timing engines (piano-dpu-panel-engines=0: DCS only)
  uint32_t displayOffMs = 20, sleepInMs = 100, sleepOutMs = 120, displayOnMs = 10;
};

struct LinkSaved {
  uint32_t ctrl = 0, dma = 0, dma1 = 0, length = 0, intr = 0, tpg = 0, trig = 0, status = 0, fifo = 0, ack = 0;
};
struct Sent {
  bool ok = false;
  const char *why = "";
  uint8_t command = 0;
  bool done[2] = {false, false};
  uint32_t doneUs = 0;          // trigger -> both done
  bool waitedBlanking = false;  // TRIG_CTRL had no BLOCK_DMA_WITHIN_FRAME: triggered in the front porch
  unsigned stores = 0, refused = 0;
  LinkSaved before[2];
  uint32_t ackAfter[2] = {0, 0}, fifoAfter[2] = {0, 0};
};

template <class Io>
Sent sendDcs(Io &io, uint8_t command, const Options &o)
{
  Sent s;
  s.command = command;
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++s.stores;
    if (!io.wr(pa, v)) ++s.refused;
  };
  if (!dcsAllowed(command)) {
    s.why = "not one of the sleep commands (0x28 0x10 0x11 0x29)";
    return s;
  }
  for (unsigned l = 0; l < 2; ++l) {
    LinkSaved &k = s.before[l];
    k.ctrl = io.rd(dsiReg(l, kDsiCtrl));
    k.dma = io.rd(dsiReg(l, kDsiCmdDmaCtrl));
    k.dma1 = io.rd(dsiReg(l, kDsiDmaCtrl1));
    k.length = io.rd(dsiReg(l, kDsiDmaLength));
    k.intr = io.rd(dsiReg(l, kDsiIntrCtrl));
    k.tpg = io.rd(dsiReg(l, kDsiTpgCtrl));
    k.trig = io.rd(dsiReg(l, kDsiTrigCtrl));
    k.fifo = io.rd(dsiReg(l, kDsiFifoStatus));
    k.ack = io.rd(dsiReg(l, kDsiAckErr));
    if ((k.ctrl & (kCtrlEnable | kCtrlVideo)) != (kCtrlEnable | kCtrlVideo)) s.why = "a DSI controller is not enabled in video mode";
    else if (k.tpg) s.why = "a DSI controller's test-pattern / command FIFO is in use";
    else if ((k.trig & kTrigDmaMask) != kTrigDmaSw) s.why = "a DSI controller's command DMA is not software-triggered (TRIG_CTRL)";
    if (*s.why) return s;
  }
  const uint64_t busyEnd = io.nowUs() + kDmaBusyTimeoutUs;
  for (unsigned l = 0; l < 2; ++l)
    while ((s.before[l].status = io.rd(dsiReg(l, kDsiStatus0))) & kStatusDmaBusy) {
      if (io.nowUs() >= busyEnd) {
        s.why = "a DSI command DMA stayed busy for 20 ms (nothing written)";
        return s;
      }
      io.delayUs(100);
    }
  const Packet pk = dcsShort(command);
  for (unsigned l = 0; l < 2; ++l) {
    const LinkSaved &k = s.before[l];
    wr(dsiReg(l, kDsiIntrCtrl), (k.intr & kIntEnables) | kIntDmaDone);
    wr(dsiReg(l, kDsiCtrl), k.ctrl | kCtrlCommand);
    wr(dsiReg(l, kDsiTpgCtrl), kTpgFifo);
    for (unsigned w = 0; w < pk.count; ++w) wr(dsiReg(l, kDsiTpgFifo), pk.words[w]);
    wr(dsiReg(l, kDsiCmdDmaCtrl), (k.dma & ~(kDmaBroadcast | kDmaMaster | kDmaLowPower)) | kDmaFromBuffer | (o.lowPower ? kDmaLowPower : 0));
    wr(dsiReg(l, kDsiDmaCtrl1), k.dma1 & ~kDmaCtrl1Bit);
    wr(dsiReg(l, kDsiDmaLength), pk.bytes);
  }
  if (s.refused) {
    s.why = "the store guard refused a store";
    return s;
  }
  // Without the hardware's block, trigger in the front porch (INTF1's lines 2242 .. vtotal - 8).
  if (!(s.before[0].trig & kTrigBlockDmaInFrame) || !(s.before[1].trig & kTrigBlockDmaInFrame)) {
    const uint32_t vt = io.rd(intfReg(1, kIntfVsyncPeriod)) / ((io.rd(intfReg(1, kIntfHsyncCtl)) >> 16) | 1u);
    s.waitedBlanking = true;
    if (vt > kVActiveEnd + 16 && !waitLines(io, kVActiveEnd + 2, vt - 8, vt, 3 * vt * kLineNs / 1000)) {
      s.why = "INTF1's line counter never reached the front porch";
      // nothing triggered: restore below as after a finished transfer
      s.done[0] = s.done[1] = true;
    }
  }
  const uint64_t t0 = io.nowUs();
  if (!*s.why) {
    const uintptr_t irq = io.irqOff();
    wr(dsiReg(0, kDsiTrigDma), 1);
    wr(dsiReg(1, kDsiTrigDma), 1);
    io.irqRestore(irq);
    for (;;) {
      for (unsigned l = 0; l < 2; ++l) s.done[l] = s.done[l] || (io.rd(dsiReg(l, kDsiIntrCtrl)) & kIntDmaDone);
      if (s.done[0] && s.done[1]) break;
      if (io.nowUs() - t0 >= kDmaDoneTimeoutUs) break;
      io.sleepMs(1);
    }
    s.doneUs = uint32_t(io.nowUs() - t0);
  }
  for (unsigned l = 0; l < 2; ++l) {
    if (!s.done[l]) continue;   // still the engine's: not touched
    const LinkSaved &k = s.before[l];
    wr(dsiReg(l, kDsiIntrCtrl), (k.intr & kIntEnables) | kIntDmaDone);
    wr(dsiReg(l, kDsiTpgCtrl), 0);
    wr(dsiReg(l, kDsiShadow[2]), 1);
    io.delayUs(1);
    wr(dsiReg(l, kDsiShadow[2]), 0);
    wr(dsiReg(l, kDsiCmdDmaCtrl), k.dma);
    wr(dsiReg(l, kDsiDmaCtrl1), k.dma1);
    wr(dsiReg(l, kDsiDmaLength), k.length);
    wr(dsiReg(l, kDsiCtrl), k.ctrl);
    s.ackAfter[l] = io.rd(dsiReg(l, kDsiAckErr));
    s.fifoAfter[l] = io.rd(dsiReg(l, kDsiFifoStatus));
  }
  if (*s.why) return s;
  if (!s.done[0] || !s.done[1]) s.why = s.done[0] ? "DSI1's command DMA did not finish within 50 ms" : "DSI0's command DMA did not finish within 50 ms";
  else if (s.refused) s.why = "the store guard refused a store";
  else {
    s.ok = true;
    s.why = "ok";
  }
  return s;
}

// --------------------------------------------------------------------------------------------------------- the engines
struct Engines {
  bool ok = false;
  const char *why = "";
  int32_t us[2] = {-1, -1};   // stop/start time per INTF (index 0 = INTF1)
  uint32_t status[2] = {0, 0};
  Rate rate;                   // after a start
  int32_t latchUs = -1;
  unsigned stores = 0, refused = 0;
};
// Stop one INTF's engine (index 1 or 2) and wait until it reports stopped.
template <class Io>
int32_t stopEngine(Io &io, unsigned intf, Engines &e)
{
  ++e.stores;
  if (!io.wr(intfReg(intf, kIntfEn), 0)) {
    ++e.refused;
    return -1;
  }
  const uint64_t t0 = io.nowUs();
  for (;;) {
    const uint32_t st = io.rd(intfReg(intf, kIntfStatus));
    e.status[intf - 1] = st;
    if (!(st & kIntfStatusOn)) return int32_t(io.nowUs() - t0);
    if (io.nowUs() - t0 >= kEngineStopUs) return -1;
    io.sleepMs(1);
  }
}
// Both on again: the CTL's INTF flush first, then INTF1 and INTF2 with nothing in between; running and in lockstep. After a failed
// stop one engine may still run: INTF2 alone is started at INTF1's frame start (its line counter wrapping), so the halves stay in
// phase; INTF1 alone (INTF2 running) cannot be aligned that way and is reported.
template <class Io>
Engines startEngines(Io &io, const LivePath &p)
{
  Engines e;
  auto wr = [&](uint64_t pa, uint32_t v) {
    ++e.stores;
    if (!io.wr(pa, v)) ++e.refused;
  };
  const bool on1 = io.rd(intfReg(1, kIntfStatus)) & kIntfStatusOn, on2 = io.rd(intfReg(2, kIntfStatus)) & kIntfStatusOn;
  wr(ctlReg(unsigned(p.ctl), kCtlIntfFlush), kIntfFlushDsi);
  wr(ctlReg(unsigned(p.ctl), kCtlFlush), kFlushIntf);
  const char *phase = nullptr;
  if (on1 && !on2) {
    const unsigned vt = p.link[0].vtotal ? p.link[0].vtotal : kVtotal120;
    if (!waitLines(io, 0, 2, vt, 3 * vt * kLineNs / 1000)) phase = "INTF1's line counter did not wrap: INTF2 started out of phase";
    const uintptr_t irq = io.irqOff();
    wr(intfReg(2, kIntfEn), 1);
    io.irqRestore(irq);
  } else if (!on1 && on2) {
    wr(intfReg(1, kIntfEn), 1);
    phase = "INTF1 started beside a running INTF2: the halves may be out of phase";
  } else if (!on1 && !on2) {
    const uintptr_t irq = io.irqOff();
    wr(intfReg(1, kIntfEn), 1);
    wr(intfReg(2, kIntfEn), 1);
    io.irqRestore(irq);
  } else {   // both still run (a stop that did not happen): cancel a pending EN = 0
    for (unsigned i = 1; i <= 2; ++i)
      if (!(io.rd(intfReg(i, kIntfEn)) & 1u)) wr(intfReg(i, kIntfEn), 1);
  }
  const uint64_t t0 = io.nowUs();
  bool on = false;
  while (!on && io.nowUs() - t0 < kEngineStartUs) {
    on = true;
    for (unsigned i = 0; i < 2; ++i) {
      e.status[i] = io.rd(intfReg(i + 1, kIntfStatus));
      on &= (e.status[i] & kIntfStatusOn) != 0;
    }
    if (!on) io.sleepMs(1);
  }
  e.us[0] = e.us[1] = on ? int32_t(io.nowUs() - t0) : -1;
  e.latchUs = waitFlush(io, unsigned(p.ctl), kFlushIntf, 60000);
  if (on) e.rate = measureRate(io, 100);
  if (e.refused) e.why = "the store guard refused a store";
  else if (!on) e.why = "a timing engine did not report running within 50 ms";
  else if (!e.rate.milliHz) e.why = "the frame counter did not move after the start";
  else if (!e.rate.lockstep) e.why = "INTF2 does not count INTF1's frames (the halves are out of step)";
  else if (phase) e.why = phase;
  else {
    e.ok = true;
    e.why = "ok";
  }
  return e;
}

// ----------------------------------------------------------------------------------------------------- off and on
struct Result {
  bool ok = false;
  const char *why = "";
  unsigned step = 0;         // the step reached (failed in, when !ok)
  bool restored = false;     // a failure undone: the panel shows again
  bool enginesStopped = false;
  Sent dcs[2];               // the two commands of the sequence
  Engines engines;
  uint64_t tUs[6] = {};      // when each step started (relative to the call)
  uint32_t stores = 0, refused = 0;
};

// Undo after a failed off: engines (if any stopped) on, sleep-out, display-on.
template <class Io>
void undoOff(Io &io, const LivePath &p, const Options &o, Result &r, bool sleeping)
{
  if (r.enginesStopped || (io.rd(intfReg(2, kIntfEn)) & 1u) == 0 || (io.rd(intfReg(1, kIntfEn)) & 1u) == 0) {
    const Engines e = startEngines(io, p);
    r.stores += e.stores;
    r.refused += e.refused;
    if (!e.ok) return;
    r.enginesStopped = false;
  }
  if (sleeping) {
    const Sent s = sendDcs(io, kDcsSleepOut, o);
    if (!s.ok) return;
    io.sleepMs(o.sleepOutMs);
  }
  r.restored = sendDcs(io, kDcsDisplayOn, o).ok;
}

template <class Io>
Result panelOff(Io &io, const LivePath &p, const Options &o)
{
  Result r;
  const uint64_t t0 = io.nowUs();
  auto fail = [&](const char *why) {
    r.why = why;
    return r;
  };
  r.step = 1;
  r.tUs[0] = io.nowUs() - t0;
  r.dcs[0] = sendDcs(io, kDcsDisplayOff, o);
  if (!r.dcs[0].ok) return fail(r.dcs[0].why);   // nothing changed (or a link still owns the command)
  io.sleepMs(o.displayOffMs);
  r.step = 2;
  r.tUs[1] = io.nowUs() - t0;
  r.dcs[1] = sendDcs(io, kDcsSleepIn, o);
  if (!r.dcs[1].ok) {
    if (r.dcs[1].done[0] && r.dcs[1].done[1]) undoOff(io, p, o, r, false);
    return fail(r.dcs[1].why);
  }
  io.sleepMs(o.sleepInMs);
  if (!o.engines) {
    r.ok = true;
    r.why = "ok (DCS only: the timing engines keep running)";
    return r;
  }
  r.step = 3;
  r.tUs[2] = io.nowUs() - t0;
  r.engines.us[1] = stopEngine(io, 2, r.engines);   // the slave first
  r.enginesStopped = true;
  if (r.engines.us[1] < 0) {
    undoOff(io, p, o, r, true);
    return fail(r.engines.refused ? "the store guard refused a store" : "INTF2's timing engine did not stop within 40 ms");
  }
  r.step = 4;
  r.tUs[3] = io.nowUs() - t0;
  r.engines.us[0] = stopEngine(io, 1, r.engines);
  if (r.engines.us[0] < 0) {
    undoOff(io, p, o, r, true);
    return fail(r.engines.refused ? "the store guard refused a store" : "INTF1's timing engine did not stop within 40 ms");
  }
  r.tUs[4] = io.nowUs() - t0;
  r.ok = true;
  r.why = "ok";
  return r;
}

template <class Io>
Result panelOn(Io &io, const LivePath &p, const Options &o, bool enginesStopped)
{
  Result r;
  const uint64_t t0 = io.nowUs();
  auto fail = [&](const char *why) {
    r.why = why;
    return r;
  };
  r.step = 1;
  if (enginesStopped) {
    r.tUs[0] = io.nowUs() - t0;
    r.engines = startEngines(io, p);
    if (!r.engines.ok) return fail(r.engines.why);
  }
  r.step = 2;
  r.tUs[1] = io.nowUs() - t0;
  r.dcs[0] = sendDcs(io, kDcsSleepOut, o);
  if (!r.dcs[0].ok) return fail(r.dcs[0].why);
  io.sleepMs(o.sleepOutMs);
  r.step = 3;
  r.tUs[2] = io.nowUs() - t0;
  r.dcs[1] = sendDcs(io, kDcsDisplayOn, o);
  if (!r.dcs[1].ok) return fail(r.dcs[1].why);
  io.sleepMs(o.displayOnMs);
  r.tUs[3] = io.nowUs() - t0;
  r.ok = true;
  r.why = "ok";
  return r;
}

}  // namespace panel
}  // namespace pianodpu
