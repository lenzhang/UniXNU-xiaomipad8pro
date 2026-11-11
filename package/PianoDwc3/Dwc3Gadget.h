// SPDX-License-Identifier: BSD-2-Clause-Patent
// A minimal DWC3 device-mode (gadget) core for the piano debug channel: one event buffer, EP0 control transfers (chapter 9 and
// the CDC-ACM class requests), and one bulk IN + one bulk OUT transfer in flight per ACM port; optionally a CDC-NCM network
// function (NCM class requests, its alternate setting, notifications, one NTB16 in flight each way). Polled, single-threaded:
// every method runs on the caller's one USB thread. Written for UniXNU-piano from the DWC3 programming model (Dwc3Regs.h); the
// takeover order and the piano-specific quirks follow Project-SunUEFI's piano driver (BSD-2-Clause-Patent, see UPSTREAM).
//
// Portable and header-only: the kernel (PianoDwc3.cpp) and the host test (tests/usb_gadget_host.cpp) instantiate it with their
// own Hal:
//   uint32_t rd(uint32_t off); void wr(uint32_t off, uint32_t v);   core registers (offsets of Dwc3Regs.h)
//   void barrier();                                                 complete DMA-memory stores/loads against MMIO (dsb sy)
//   void udelay(unsigned us);
//   uint64_t nowUs();                                               a monotonic clock (the EP0 watchdog, snapshots)
//   uint8_t *dma(); uint64_t dmaPhys();                             the non-cacheable region of Layout (CPU / device view)
//   unsigned txFill(unsigned port, uint8_t *dst, unsigned cap);     bytes to send on the port's bulk IN (0: nothing)
//   unsigned rxSpace(unsigned port);                                room for bulk OUT data
//   void rxData(unsigned port, const uint8_t *src, unsigned len);
//   void lineState(unsigned port, uint16_t state);                  SET_CONTROL_LINE_STATE (bit 0 DTR, bit 1 RTS)
//   void note(const char *fmt, ...);                                diagnostics
// and for the network function (bind(..., net = true); never called without it):
//   unsigned netTxNext(uint8_t *dst, unsigned room);                the next frame to the host into dst if it has at most room
//                                                                   bytes (it stays queued otherwise); its length, 0 for none
//   unsigned netRxSpace();                                          room for frames from the host (an NTB's worth is asked for)
//   bool netRxFrame(const uint8_t *src, unsigned len);              one frame from the host (false: no room, dropped)
//   void netRxDone();                                               after the frames of one NTB
//   void netLink(bool up, uint32_t bitsPerSecond);                  the host selected the data interface's alternate 1 (or left it)
#pragma once
#include "Dwc3Regs.h"
#include "UsbDescriptors.h"
#include "Ncm.h"
#include "TxFifoPlan.h"

namespace pianousb {

// One physically contiguous, non-cacheable region (128 KiB: three ports and the network function).
struct Layout {
  static constexpr uint32_t kBytes = 0x20000;
  static constexpr uint32_t kEvents = 0x0000, kEventBytes = 0x1000;     // event buffer 0
  static constexpr uint32_t kTrbs = 0x1000;                             // physical endpoint n: two TRBs at kTrbs + 32 n
  static constexpr uint32_t kSetup = 0x1800;                            // SETUP packet (8 bytes)
  static constexpr uint32_t kZero = 0x1840;                             // target of zero-length TRBs
  static constexpr uint32_t kEp0 = 0x2000, kEp0Bytes = 0x1000;          // EP0 data stage
  static constexpr uint32_t kPortBase = 0x3000, kPortStride = 0x6000;   // per ACM port:
  static constexpr uint32_t kInOff = 0x0000, kInBytes = 0x4000;         //   bulk IN (16 KiB)
  static constexpr uint32_t kOutOff = 0x4000, kOutBytes = 0x1000;       //   bulk OUT (4 KiB, a multiple of 512 and 64)
  static constexpr uint32_t portOff(unsigned p) { return kPortBase + p * kPortStride; }
  static constexpr uint32_t kNetNotify = 0x15000;                       // network function: one notification (16 bytes),
  static constexpr uint32_t kNetIn = 0x16000, kNetOut = 0x1a000;        //   NTB IN and NTB OUT (ncm::kNtbMax each)
};
static_assert(Layout::kPortBase + kPorts * Layout::kPortStride <= Layout::kNetNotify && Layout::kNetNotify + 64 <= Layout::kNetIn &&
              Layout::kNetIn + ncm::kNtbMax <= Layout::kNetOut && Layout::kNetOut + ncm::kNtbMax <= Layout::kBytes, "DMA layout");
static_assert(ncm::kNtbMax % 512 == 0 && Layout::kTrbs + 32 * (2 * kNetBulkEp + 2) <= Layout::kSetup, "NTB OUT size, TRB area");

// EP0 OUT/IN, then per port and for the network function (the fifth slot): (unused OUT), notify IN, bulk OUT, bulk IN.
constexpr unsigned kPhys = 2 + 4 * (kPorts + 1);
constexpr unsigned kNetNotifyPhys = physOf(kNetNotifyEp, true), kNetOutPhys = physOf(kNetBulkEp, false),
                   kNetInPhys = physOf(kNetBulkEp, true);
static_assert(kNetInPhys < kPhys, "physical endpoints");
// The network function's IN endpoints (7, 8) are the highest: a core needs this many IN endpoints for it.
constexpr unsigned kNetInEndpoints = kNetBulkEp + 1u;

struct GadgetStats {
  uint32_t events, deviceEvents, resets, connects, disconnects, linkChanges, erratic, overflows, unknown;
  uint32_t setups, stalls, cmdErrors, cmdTimeouts, startErrors, stale, ep0Timeouts, reconnects;
  uint32_t inTransfers, outTransfers;
  uint64_t inBytes, outBytes;
  uint32_t lastSetupLo, lastSetupHi, lastCmdFailure;
  uint8_t speed, address, configuration, linkState;
};

template <class Hal>
class Gadget {
public:
  GadgetStats stats;
  ncm::NetStats netStats;   // the network function's counters (kept across bus resets)
  uint16_t lineState[kPorts];
  uint8_t lineCoding[kPorts][7];

  // traceEvents: how many events after each bus reset are logged one by one (with the EP0 state), 0 for none. net: the CDC-NCM
  // function after the ports (piano-usb-net=1); without it the device is the ACM-only one, descriptors and behaviour unchanged.
  void bind(Hal *hal, unsigned ports, uint16_t product, uint32_t traceEvents = 0, bool net = false)
  {
    h = hal;
    portCount = ports < 1 ? 1 : ports > kPorts ? kPorts : ports;
    productId = product;
    traceLimit = traceEvents;
    netOn = net;
    fifoPlan = TxFifoPlan{};
    netStats = ncm::NetStats{};
    netStats.ntbInSize = ncm::kNtbMax;
    netAlt = 0;
    for (unsigned p = 0; p < kPorts; ++p) {
      static const uint8_t k115200[7] = {0x00, 0xc2, 0x01, 0x00, 0, 0, 8};   // 115200 8N1 (only reported, never used)
      for (unsigned i = 0; i < 7; ++i) lineCoding[p][i] = k115200[i];
      lineState[p] = 0;
    }
  }
  unsigned ports() const { return portCount; }
  bool net() const { return netOn; }
  bool configured() const { return stats.configuration == 1; }
  bool netLinkUp() const { return netOn && configured() && netAlt == 1; }
  unsigned interfaces() const { return 2 * portCount + (netOn ? 2 : 0); }
  // TX FIFO sizes written by initDevice() (the network function's; planTxFifos), before the core runs.
  void setTxFifoPlan(const TxFifoPlan &plan) { fifoPlan = plan; }

  // DCTL.CSFTRST with both PHY interfaces awake (a soft reset needs their clocks; the firmware's USB shutdown can leave the
  // UTMI side in suspend). dwc31: DWC_usb31/32 (GSNPSID 0x3331/0x3332): 20 ms steps up to 200 ms, then 50 ms settling.
  bool softReset(bool dwc31, uint32_t *waitedUs)
  {
    using namespace dwc3;
    wakePhy();
    h->udelay(1000);
    h->wr(DCTL, (h->rd(DCTL) & ~(DCTL_RUN_STOP | DCTL_ULSTCHNGREQ_MASK)) | DCTL_CSFTRST);
    const uint32_t step = dwc31 ? 20000 : 10, limit = dwc31 ? 200000 : 100000;
    uint32_t waited = 0;
    while (h->rd(DCTL) & DCTL_CSFTRST) {
      if (waited >= limit) { *waitedUs = waited; return false; }
      h->udelay(step);
      waited += step;
    }
    if (dwc31) h->udelay(50000);
    wakePhy();
    *waitedUs = waited;
    return true;
  }

  // Device mode, high (or full) speed only: event buffer, EP0 and the transfer resources. The core must be halted.
  bool init()
  {
    using namespace dwc3;
    uint8_t *m = h->dma();
    for (uint32_t i = 0; i < Layout::kEventBytes; ++i) m[Layout::kEvents + i] = 0;
    const uint64_t ring = h->dmaPhys() + Layout::kEvents;
    h->barrier();
    h->wr(GEVNTADRLO, uint32_t(ring));
    h->wr(GEVNTADRHI, uint32_t(ring >> 32));
    h->wr(GEVNTSIZ, GEVNTSIZ_INTMASK | Layout::kEventBytes);   // polled: the line stays quiet, events are still written
    const uint32_t count = h->rd(GEVNTCOUNT) & 0xffffu;
    if (count) h->wr(GEVNTCOUNT, count);
    ringPos = 0;
    return initDevice();
  }

  // The device side again (role, speed, EP0, transfer resources) with the event buffer as it is: after init(), and for a
  // reconnect (the core keeps its event write position, as Linux relies on when it toggles RUN_STOP).
  bool initDevice()
  {
    using namespace dwc3;
    h->wr(GCTL, (h->rd(GCTL) & ~GCTL_PRTCAPDIR_MASK) | GCTL_PRTCAP_DEVICE | GCTL_DSBLCLKGTNG);
    h->wr(DCFG, (h->rd(DCFG) & ~(DCFG_DEVSPD_MASK | DCFG_DEVADDR_MASK | DCFG_NUMP_MASK | DCFG_LPM_CAP)) | DCFG_SPEED_HS | dcfgNump(16));
    for (unsigned i = 0; i < kPhys; ++i) ep[i] = EpState{};
    netAlt = netAltNew = 0;
    netNotifyPending = 0;
    pendNetAlt = false;
    phase = kIdle;
    ep0Mps = 64;
    highSpeed = true;
    setupNotes = 0;
    stats = GadgetStats{};
    traceBudget = traceLimit;
    recentPos = 0;
    ep0Since = lastSetupUs = h->nowUs();
    lastSnapshot = 0;
    snapshots = 0;
    if (!cmd(0, CMD_DEPSTARTCFG, 0, 0, 0, nullptr)) return false;
    for (unsigned phys = 0; phys < 2; ++phys)
      if (!configureEp(phys, EP_CONTROL, ep0Mps, DEPCFG_ACTION_INIT, 0) || !cmd(phys, CMD_SETTRANSFRESOURCE, 1, 0, 0, nullptr)) return false;
    for (unsigned phys = 2; phys < 2 + 4 * portCount; ++phys)
      if (!cmd(phys, CMD_SETTRANSFRESOURCE, 1, 0, 0, nullptr)) return false;
    if (netOn)
      for (unsigned phys = kNetNotifyPhys; phys <= kNetInPhys; ++phys)
        if (!cmd(phys, CMD_SETTRANSFRESOURCE, 1, 0, 0, nullptr)) return false;
    for (unsigned n = 1; n < fifoPlan.fifos; ++n)   // the core is halted and no IN endpoint is enabled
      if (fifoPlan.value[n]) h->wr(GTXFIFOSIZ0 + 4 * n, fifoPlan.value[n]);
    h->wr(DALEPENA, 3);
    h->wr(DEVTEN, DEVTEN_DISCONN | DEVTEN_USBRST | DEVTEN_CONNECTDONE | DEVTEN_ULSTCNG | DEVTEN_ERRTICERR);
    return true;
  }

  // Arm the first SETUP, then RUN_STOP. waitMs bounds the wait for DSTS.DEVCTRLHLT to clear.
  bool run(unsigned waitMs)
  {
    using namespace dwc3;
    if (!armSetup()) return false;
    h->wr(DCTL, (h->rd(DCTL) & ~(DCTL_U1U2_MASK | DCTL_ULSTCHNGREQ_MASK | DCTL_TSTCTRL_MASK)) | DCTL_RUN_STOP);
    for (unsigned i = 0; i < waitMs * 10; ++i) {
      if (!(h->rd(DSTS) & DSTS_DEVCTRLHLT)) return true;
      h->udelay(100);
    }
    return !(h->rd(DSTS) & DSTS_DEVCTRLHLT);
  }

  // RUN_STOP off, then wait for the halt (true when halted).
  bool halt(unsigned waitMs)
  {
    using namespace dwc3;
    h->wr(DCTL, h->rd(DCTL) & ~DCTL_RUN_STOP);
    for (unsigned i = 0; i < waitMs * 10; ++i) {
      if (h->rd(DSTS) & DSTS_DEVCTRLHLT) return true;
      h->udelay(100);
    }
    return (h->rd(DSTS) & DSTS_DEVCTRLHLT) != 0;
  }

  // Process the pending events; returns how many.
  unsigned poll()
  {
    using namespace dwc3;
    uint32_t count = h->rd(GEVNTCOUNT) & 0xffffu;
    if (!count) return 0;
    if (count > Layout::kEventBytes || (count & 3)) {
      h->note("event count %#x out of range", count);
      count = count > Layout::kEventBytes ? Layout::kEventBytes : count & ~3u;
      if (!count) return 0;
    }
    h->barrier();
    const volatile uint32_t *ring = reinterpret_cast<const volatile uint32_t *>(h->dma() + Layout::kEvents);
    for (uint32_t n = 0; n < count; n += 4) {
      const uint32_t e = ring[ringPos / 4];
      ringPos = (ringPos + 4) % Layout::kEventBytes;
      handle(e);
    }
    h->wr(GEVNTCOUNT, count);
    return count / 4;
  }

  // Keep one bulk OUT and one bulk IN transfer queued per port while configured.
  void pump()
  {
    if (!configured()) return;
    for (unsigned p = 0; p < portCount; ++p) {
      const unsigned out = physOf(bulkEp(p), false), in = physOf(bulkEp(p), true);
      if (!ep[out].busy && ep[out].configured && h->rxSpace(p) >= Layout::kOutBytes)
        startXfer(out, Layout::portOff(p) + Layout::kOutOff, Layout::kOutBytes, dwc3::TRBCTL_NORMAL, false);
      if (!ep[in].busy && ep[in].configured) {
        const unsigned n = h->txFill(p, h->dma() + Layout::portOff(p) + Layout::kInOff, Layout::kInBytes);
        // A transfer ending on a packet boundary gets a zero-length packet, so the host's read completes now.
        if (n) startXfer(in, Layout::portOff(p) + Layout::kInOff, n, dwc3::TRBCTL_NORMAL, n % ep[in].mps == 0);
      }
    }
    if (netOn) netPump();
  }

  bool inBusy(unsigned port) const { return ep[physOf(bulkEp(port), true)].busy; }

  // Call from the poll loop. EP0 watchdog: a control transfer that has not moved for 1 s (no event for its data or status
  // stage) is logged with the whole EP0 state, ended, and SETUP is armed again, so the host's retry gets through. While a host
  // is attached but has not configured the device: a snapshot every 2 s (at most 30 per bus reset).
  void tick()
  {
    const uint64_t now = h->nowUs();
    if (!stats.connects) return;
    if ((phase == kData || phase == kWaitStatus || phase == kStatus) && now - ep0Since > 1000000) {
      ++stats.ep0Timeouts;
      snapshot("EP0 stuck 1 s: ending it, SETUP again");
      endXfer(0);
      endXfer(1);
      pendAddress = pendConfig = pendNetAlt = false;
      pendLinePort = outPort = -1;
      armSetup();
    } else if (phase == kSetup && !ep[0].busy && now - ep0Since > 100000) {
      snapshot("SETUP not armed: arming");
      armSetup();
    }
    if (!configured() && snapshots < 30 && now - lastSnapshot > 2000000) {
      lastSnapshot = now;
      ++snapshots;
      snapshot("not configured yet");
    }
    // A host that asked for something, then nothing for 10 s, and never configured us: disconnect and connect again (at
    // most 3 times), so it enumerates once more and the trace shows whether it stops at the same request.
    if (stats.setups && !configured() && phase == kSetup && ep[0].busy && now - lastSetupUs > 10000000 && reconnects < 3) {
      ++reconnects;
      snapshot("no request for 10 s and not configured: soft disconnect, connect again");
      reconnect();
    }
  }

  // RUN_STOP off, the endpoints and the event buffer set up again, RUN_STOP on (the counters are kept).
  void reconnect()
  {
    halt(100);
    busReset(false);
    const GadgetStats kept = stats;   // after busReset: address and configuration are 0 again
    h->udelay(300000);   // long enough for the host to see the detach
    poll();              // whatever the halt produced, with the state of now
    const bool ok = initDevice();
    stats = kept;
    stats.reconnects = reconnects;
    if (!ok || !run(50)) h->note("reconnect: the core did not restart (DSTS %08x)", h->rd(dwc3::DSTS));
  }

  void snapshot(const char *why)
  {
    using namespace dwc3;
    const volatile uint32_t *t0 = trb(0), *t1 = trb(1);
    h->note("%s: phase %s (%s, status on %u) busy %u/%u since %llu ms; trb0 %08x %08x %08x %08x trb1 %08x %08x %08x %08x;"
            " DEPCMD0 %08x DEPCMD1 %08x DSTS %08x DCFG %08x DALEPENA %08x GEVNTCOUNT %08x; events %u setups %u stalls %u stale %u;"
            " last events %08x %08x %08x %08x %08x %08x",
            why, phaseName(), threeStage ? "3-stage" : "2-stage", statusPhys, ep[0].busy, ep[1].busy,
            (unsigned long long)((h->nowUs() - ep0Since) / 1000), t0[0], t0[1], t0[2], t0[3], t1[0], t1[1], t1[2], t1[3],
            h->rd(depcmdBase(0) + 12), h->rd(depcmdBase(1) + 12), h->rd(DSTS), h->rd(DCFG), h->rd(DALEPENA), h->rd(GEVNTCOUNT),
            stats.events, stats.setups, stats.stalls, stats.stale, recent[(recentPos - 6) & 7], recent[(recentPos - 5) & 7],
            recent[(recentPos - 4) & 7], recent[(recentPos - 3) & 7], recent[(recentPos - 2) & 7], recent[(recentPos - 1) & 7]);
  }

private:
  enum Phase : uint8_t { kIdle, kSetup, kData, kWaitStatus, kStatus };
  enum Kind : uint8_t { kStall, kNoData, kIn, kOut };
  struct Reply { Kind kind; uint16_t len; };
  struct EpState { bool busy, configured, zlp; uint8_t resource; uint16_t mps; uint32_t posted; };

  Hal *h;
  unsigned portCount;
  uint16_t productId;
  uint32_t ringPos;
  uint32_t traceLimit, traceBudget;   // events still logged one by one since the last bus reset
  uint32_t recent[8];                 // the last events, for snapshots
  uint32_t recentPos;
  uint64_t ep0Since, lastSnapshot;    // EP0's last progress; the last periodic snapshot
  uint64_t lastSetupUs;               // the last SETUP packet
  uint32_t snapshots, reconnects;
  EpState ep[kPhys];
  Phase phase;
  bool threeStage, highSpeed;
  uint8_t statusPhys, dataPhys;
  uint16_t ep0Mps;
  uint32_t setupNotes;
  // applied when the status stage completes
  bool pendAddress, pendConfig;
  uint8_t newAddress, newConfig;
  int pendLinePort;
  uint16_t pendLineValue;
  // OUT data stage target (kNetSlot: a network function request)
  int outPort;
  uint8_t outRequest;
  // the network function
  bool netOn;
  TxFifoPlan fifoPlan;
  uint8_t netAlt, netAltNew;   // the data interface's alternate setting; the one SET_INTERFACE asked for (after its status stage)
  bool pendNetAlt;
  uint8_t netNotifyPending;    // kNotifySpeed / kNotifyConnect still to send
  uint16_t netSeq;             // wSequence of the next NTB to the host
  enum : uint8_t { kNotifySpeed = 1, kNotifyConnect = 2 };

  void wakePhy()
  {
    using namespace dwc3;
    h->wr(GUSB2PHYCFG, h->rd(GUSB2PHYCFG) & ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM));
    h->wr(GUSB3PIPECTL, h->rd(GUSB3PIPECTL) & ~GUSB3PIPECTL_SUSPHY);
  }

  volatile uint32_t *trb(unsigned phys) { return reinterpret_cast<volatile uint32_t *>(h->dma() + Layout::kTrbs + 32 * phys); }

  bool cmd(unsigned phys, uint32_t command, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *result)
  {
    using namespace dwc3;
    const uint32_t base = depcmdBase(phys);
    h->wr(base + 0, p2);
    h->wr(base + 4, p1);
    h->wr(base + 8, p0);
    h->barrier();
    h->wr(base + 12, command | DEPCMD_CMDACT);
    for (unsigned i = 0; i < 1000; ++i) {
      const uint32_t v = h->rd(base + 12);
      if (!(v & DEPCMD_CMDACT)) {
        if (result) *result = v;
        if (depcmdStatus(v)) {
          ++stats.cmdErrors;
          stats.lastCmdFailure = phys << 24 | (command & 0xf) << 16 | depcmdStatus(v);
          h->note("ep %u command %#x: status %u (%08x)", phys, command, depcmdStatus(v), v);
          return false;
        }
        return true;
      }
      h->udelay(10);
    }
    ++stats.cmdTimeouts;
    stats.lastCmdFailure = phys << 24 | (command & 0xf) << 16 | 0xffff;
    h->note("ep %u command %#x: timeout", phys, command);
    return false;
  }

  bool configureEp(unsigned phys, unsigned type, unsigned mps, uint32_t action, unsigned intervalM1)
  {
    using namespace dwc3;
    uint32_t p0 = depcfgType(type) | depcfgMps(mps) | action;
    if ((phys & 1) && type != EP_CONTROL) p0 |= depcfgFifo(phys >> 1);   // IN endpoint n uses TX FIFO n
    uint32_t p1 = DEPCFG_XFER_COMPLETE_EN | depcfgEpNumber(phys);
    if (type == EP_CONTROL) p1 |= DEPCFG_XFER_NOT_READY_EN;
    if (type == EP_INTERRUPT) p1 |= depcfgInterval(intervalM1);
    if (!cmd(phys, CMD_SETEPCONFIG, p0, p1, 0, nullptr)) return false;
    ep[phys].mps = uint16_t(mps);
    return true;
  }

  bool startXfer(unsigned phys, uint32_t off, uint32_t len, unsigned ctl, bool zlp)
  {
    using namespace dwc3;
    volatile uint32_t *t = trb(phys);
    const uint64_t buffer = h->dmaPhys() + off, zero = h->dmaPhys() + Layout::kZero;
    t[0] = uint32_t(buffer);
    t[1] = uint32_t(buffer >> 32);
    t[2] = len;
    if (zlp) {
      t[4] = uint32_t(zero);
      t[5] = uint32_t(zero >> 32);
      t[6] = 0;
      t[7] = TRB_HWO | TRB_LST | TRB_IOC | TRB_ISP_IMI | trbCtl(ctl);
      t[3] = TRB_HWO | TRB_CHN | trbCtl(ctl);
    } else {
      t[3] = TRB_HWO | TRB_LST | TRB_IOC | TRB_ISP_IMI | trbCtl(ctl);
    }
    h->barrier();
    const uint64_t at = h->dmaPhys() + Layout::kTrbs + 32 * phys;
    uint32_t v = 0;
    ep[phys].busy = true;   // even a refused START may have read the TRB: owned until END or reset
    ep[phys].posted = len;
    ep[phys].zlp = zlp;
    if (!cmd(phys, CMD_STARTTRANSFER, uint32_t(at >> 32), uint32_t(at), 0, &v)) {
      ++stats.startErrors;
      endXfer(phys);
      return false;
    }
    ep[phys].resource = uint8_t(depcmdResource(v));
    return true;
  }

  // END TRANSFER without the completion event: the 1 ms the programming model allows for it, then the TRB is ours again.
  void endXfer(unsigned phys)
  {
    using namespace dwc3;
    if (!ep[phys].busy) return;
    cmd(phys, CMD_ENDTRANSFER | DEPCMD_FORCERM | depcmdParam(ep[phys].resource), 0, 0, 0, nullptr);
    h->udelay(1000);
    ep[phys].busy = false;
  }

  bool armSetup()
  {
    phase = kSetup;
    ep0Since = h->nowUs();
    volatile uint8_t *s = h->dma() + Layout::kSetup;
    for (unsigned i = 0; i < 8; ++i) s[i] = 0;
    if (startXfer(0, Layout::kSetup, 8, dwc3::TRBCTL_CONTROL_SETUP, false)) return true;
    h->note("SETUP could not be armed (DEPCMD0 %08x); tick() retries", h->rd(dwc3::depcmdBase(0) + 12));
    return false;
  }

  void stallRestart()
  {
    ++stats.stalls;
    endXfer(0);
    endXfer(1);
    cmd(0, dwc3::CMD_SETSTALL, 0, 0, 0, nullptr);   // the core clears it with the next SETUP
    pendAddress = pendConfig = pendNetAlt = false;
    pendLinePort = outPort = -1;
    armSetup();
  }

  const char *phaseName() const
  {
    static const char *const names[] = {"idle", "setup", "data", "wait-status", "status"};
    return phase <= kStatus ? names[phase] : "?";
  }

  // One line per event while traceBudget lasts: the raw word, what it is, and the EP0 state it arrives in (before handling).
  void trace(uint32_t e)
  {
    using namespace dwc3;
    if (!traceBudget) return;
    --traceBudget;
    if (e & 1) {
      static const char *const names[] = {"disconnect", "reset", "connect-done", "link-change", "wakeup", "hibernation", "suspend",
                                          "sof", "?", "erratic", "command-complete", "overflow", "vendor-test", "?", "?", "?"};
      h->note("ev %08x device %s info %x (DSTS %08x)", e, names[devEventType(e)], devEventInfo(e), h->rd(DSTS));
      return;
    }
    static const char *const types[] = {"?", "xfer-complete", "in-progress", "not-ready", "?", "?", "stream", "command-complete",
                                        "?", "?", "?", "?", "?", "?", "?", "?"};
    const unsigned phys = epEventPhys(e), type = epEventType(e), status = epEventStatus(e);
    if (phys < kPhys && (type == EPT_XFER_COMPLETE || type == EPT_CMD_COMPLETE)) {
      const volatile uint32_t *t = trb(phys);
      h->note("ev %08x ep %u %s status %x | phase %s busy %u/%u | trb %08x %08x (posted %u)", e, phys, types[type], status,
              phaseName(), ep[0].busy, ep[1].busy, t[2], t[3], ep[phys].posted);
    } else if (type == EPT_XFER_NOT_READY) {
      h->note("ev %08x ep %u not-ready %s%s | phase %s busy %u/%u", e, phys, (status & 3) == EP0_NOTREADY_STATUS ? "status" :
              (status & 3) == EP0_NOTREADY_DATA ? "data" : "-", status & 8 ? " (transfer active)" : "", phaseName(), ep[0].busy,
              ep[1].busy);
    } else {
      h->note("ev %08x ep %u %s status %x | phase %s", e, phys, types[type], status, phaseName());
    }
  }

  void handle(uint32_t e)
  {
    using namespace dwc3;
    ++stats.events;
    recent[recentPos++ & 7] = e;
    trace(e);
    if (e & 1) {
      if (eventIsDevice(e)) deviceEvent(e);
      else { ++stats.unknown; h->note("event %08x ignored", e); }
      return;
    }
    const unsigned phys = epEventPhys(e), type = epEventType(e), status = epEventStatus(e);
    if (phys >= kPhys) { ++stats.unknown; h->note("event %08x for endpoint %u ignored", e, phys); return; }
    if (phys < 2) { ep0Event(phys, type, status); return; }
    if (type == EPT_XFER_COMPLETE) bulkComplete(phys);
  }

  void deviceEvent(uint32_t e)
  {
    using namespace dwc3;
    ++stats.deviceEvents;
    switch (devEventType(e)) {
    case DEVT_DISCONNECT:
      ++stats.disconnects;
      h->note("disconnect");
      busReset(false);
      break;
    case DEVT_RESET:
      ++stats.resets;
      h->note("bus reset (DSTS %08x)", h->rd(DSTS));
      busReset(true);
      break;
    case DEVT_CONNECT_DONE: {
      ++stats.connects;
      const uint32_t speed = h->rd(DSTS) & DSTS_CONNECTSPD_MASK;
      stats.speed = uint8_t(speed);
      highSpeed = speed == SPEED_HS;
      ep0Mps = speed == SPEED_SS ? 512 : 64;
      h->note("connected: %s", speed == SPEED_HS ? "high speed" : speed == SPEED_SS ? "super speed" : "full speed");
      configureEp(0, EP_CONTROL, ep0Mps, DEPCFG_ACTION_MODIFY, 0);
      configureEp(1, EP_CONTROL, ep0Mps, DEPCFG_ACTION_MODIFY, 0);
      if (!ep[0].busy && !ep[1].busy) armSetup();
      break;
    }
    case DEVT_LINK_CHANGE:
      ++stats.linkChanges;
      stats.linkState = uint8_t(devEventInfo(e) & 0xf);
      break;
    case DEVT_ERRATIC:
      ++stats.erratic;
      h->note("erratic error (DSTS %08x)", h->rd(DSTS));
      break;
    case DEVT_OVERFLOW:
      ++stats.overflows;
      h->note("event buffer overflow");
      break;
    default:
      break;
    }
  }

  void busReset(bool rearm)
  {
    using namespace dwc3;
    for (unsigned phys = 2; phys < kPhys; ++phys) {
      endXfer(phys);
      ep[phys].configured = false;
    }
    h->wr(DALEPENA, 3);
    endXfer(0);
    endXfer(1);
    h->wr(DCFG, h->rd(DCFG) & ~DCFG_DEVADDR_MASK);
    stats.address = stats.configuration = 0;
    pendAddress = pendConfig = pendNetAlt = false;
    pendLinePort = outPort = -1;
    netDown();
    setupNotes = 0;
    snapshots = 0;
    if (stats.resets + stats.disconnects <= 8) traceBudget = traceLimit;   // a host that resets in a loop does not flood the log
    for (unsigned p = 0; p < kPorts; ++p)
      if (lineState[p]) { lineState[p] = 0; h->lineState(p, 0); }
    phase = kIdle;
    if (rearm) armSetup();
  }

  void ep0Event(unsigned phys, unsigned type, unsigned status)
  {
    using namespace dwc3;
    if (type == EPT_XFER_COMPLETE) {
      volatile uint32_t *t = trb(phys);
      if (!ep[phys].busy || (t[3] & TRB_HWO)) { ++stats.stale; return; }
      ep[phys].busy = false;
      ep0Since = h->nowUs();
      const uint32_t residual = trbResidual(t[2]), trbsts = trbStatus(t[2]);
      switch (phase) {
      case kSetup:
        if (phys == 0) setup();
        else ++stats.stale;
        return;
      case kData:
        if (trbsts == TRBSTS_SETUP_PENDING) { armSetup(); return; }   // the host gave up on this request
        if (phys != dataPhys) { ++stats.stale; return; }
        if (phys == 0 && outPort >= 0) dataOut(residual <= ep[0].posted ? ep[0].posted - residual : 0);
        phase = kWaitStatus;
        return;
      case kStatus:
        if (phys != statusPhys) { ++stats.stale; return; }
        statusDone();
        armSetup();
        return;
      default:
        ++stats.stale;
        return;
      }
    }
    if (type != EPT_XFER_NOT_READY) return;
    if ((status & 3) == EP0_NOTREADY_DATA && phase == kData && phys != dataPhys) {
      h->note("data stage in the other direction (ep %u, expected %u): stall", phys, dataPhys);
      stallRestart();
      return;
    }
    if ((status & 3) == EP0_NOTREADY_STATUS && (phase == kWaitStatus || phase == kData)) {
      ep0Since = h->nowUs();
      if (phys != statusPhys) { h->note("status stage on the wrong direction (ep %u, expected %u)", phys, statusPhys); stallRestart(); return; }
      if (phase == kData) endXfer(dataPhys);   // the host moved on: a short data stage
      phase = kStatus;
      if (!startXfer(phys, Layout::kZero, 0, threeStage ? TRBCTL_CONTROL_STATUS3 : TRBCTL_CONTROL_STATUS2, false)) stallRestart();
    }
  }

  void setup()
  {
    using namespace dwc3;
    const volatile uint8_t *s = h->dma() + Layout::kSetup;
    uint8_t u[8];
    for (unsigned i = 0; i < 8; ++i) u[i] = s[i];
    ++stats.setups;
    lastSetupUs = h->nowUs();
    stats.lastSetupLo = uint32_t(u[0]) | uint32_t(u[1]) << 8 | uint32_t(u[2]) << 16 | uint32_t(u[3]) << 24;
    stats.lastSetupHi = uint32_t(u[4]) | uint32_t(u[5]) << 8 | uint32_t(u[6]) << 16 | uint32_t(u[7]) << 24;
    const uint16_t length = uint16_t(u[6] | u[7] << 8);
    pendAddress = pendConfig = pendNetAlt = false;
    pendLinePort = outPort = -1;
    const Reply r = dispatch(u);
    // What goes on the wire: an IN data stage is the reply cut to wLength (+ a zero-length packet when it ends short of
    // wLength on a packet boundary); an OUT data stage is received into whole packets.
    const uint16_t n = r.len < length ? r.len : length;
    const bool zlp = r.kind == kIn && n < length && n && n % ep0Mps == 0;
    const uint32_t outSize = (uint32_t(length) + ep0Mps - 1) / ep0Mps * ep0Mps;
    if (setupNotes < 48) {
      ++setupNotes;
      if (r.kind == kIn)
        h->note("setup %02x %02x %04x %04x %u -> in %u of %u bytes%s (%u packets of %u)", u[0], u[1], u[2] | u[3] << 8,
                u[4] | u[5] << 8, length, n, r.len, zlp ? " + zero-length packet" : "", (n + ep0Mps - 1) / ep0Mps, ep0Mps);
      else
        h->note("setup %02x %02x %04x %04x %u -> %s", u[0], u[1], u[2] | u[3] << 8, u[4] | u[5] << 8, length,
                r.kind == kStall ? "stall" : r.kind == kNoData ? "status only" : "out");
    }
    switch (r.kind) {
    case kStall:
      stallRestart();
      return;
    case kNoData:
      threeStage = false;
      statusPhys = 1;
      phase = kWaitStatus;
      return;
    case kIn:
      threeStage = true;
      dataPhys = 1;
      statusPhys = 0;
      phase = kData;
      if (!startXfer(1, Layout::kEp0, n, TRBCTL_CONTROL_DATA, zlp)) stallRestart();
      return;
    case kOut:
      threeStage = true;
      dataPhys = 0;
      statusPhys = 1;
      phase = kData;
      if (!startXfer(0, Layout::kEp0, outSize, TRBCTL_CONTROL_DATA, false)) stallRestart();
      return;
    }
  }

  // Bulk/interrupt endpoint number -> physical endpoint of this gadget (or kPhys when not ours).
  unsigned physFromAddress(uint16_t address) const
  {
    const unsigned number = address & 0x0f, in = (address & 0x80) != 0;
    for (unsigned p = 0; p < portCount; ++p) {
      if (number == notifyEp(p) && in) return physOf(notifyEp(p), true);
      if (number == bulkEp(p)) return physOf(bulkEp(p), in);
    }
    if (netOn && number == kNetNotifyEp && in) return kNetNotifyPhys;
    if (netOn && number == kNetBulkEp) return physOf(kNetBulkEp, in);
    return number == 0 ? in : kPhys;
  }

  Reply dispatch(const uint8_t *u)
  {
    using namespace dwc3;
    const uint8_t type = u[0], request = u[1];
    const uint16_t value = uint16_t(u[2] | u[3] << 8), index = uint16_t(u[4] | u[5] << 8), length = uint16_t(u[6] | u[7] << 8);
    uint8_t *data = h->dma() + Layout::kEp0;
    const unsigned cap = Layout::kEp0Bytes;
    const Reply stall = {kStall, 0}, none = {kNoData, 0};
    if ((type & 0x60) == 0x00) {   // standard
      const unsigned recipient = type & 0x1f;
      const bool in = type & 0x80;
      switch (request) {
      case 0x00:   // GET_STATUS
        if (!in || length < 2) return stall;
        data[0] = recipient == 0 ? 1 : 0;   // self powered
        data[1] = 0;
        if (recipient == 2 && physFromAddress(index) >= kPhys) return stall;
        return {kIn, 2};
      case 0x01:   // CLEAR_FEATURE
      case 0x03:   // SET_FEATURE
        if (in || length) return stall;
        if (recipient == 2 && value == 0) {   // ENDPOINT_HALT
          const unsigned phys = physFromAddress(index);
          if (phys >= kPhys || (phys >= 2 && !ep[phys].configured)) return stall;
          if (phys >= 2) cmd(phys, request == 0x01 ? CMD_CLEARSTALL : CMD_SETSTALL, 0, 0, 0, nullptr);
          return none;
        }
        if (recipient == 0 && value == 1) return none;   // DEVICE_REMOTE_WAKEUP: accepted, never used
        return stall;
      case 0x05:   // SET_ADDRESS: DCFG before the status stage (the core switches address after it)
        if (in || length || recipient || value > 127) return stall;
        h->wr(DCFG, (h->rd(DCFG) & ~DCFG_DEVADDR_MASK) | dcfgAddress(value));
        pendAddress = true;
        newAddress = uint8_t(value);
        return none;
      case 0x06: {   // GET_DESCRIPTOR
        if (!in) return stall;
        if (!length) return none;
        const uint8_t kind = uint8_t(value >> 8), which = uint8_t(value);
        unsigned n = 0;
        switch (kind) {
        case 1: n = which ? 0 : deviceDescriptor(data, cap, productId); break;
        case 2: n = which ? 0 : configDescriptor(data, cap, 2, highSpeed, portCount, netOn); break;
        case 3: n = stringDescriptor(data, cap, which, netOn ? kStringCount : kStringCountAcm); break;
        case 6: n = which ? 0 : qualifierDescriptor(data, cap); break;
        case 7: n = which ? 0 : configDescriptor(data, cap, 7, !highSpeed, portCount, netOn); break;
        default: n = 0; break;   // BOS (USB 2.0 device) and the rest: stall
        }
        if (!n || n > cap) return stall;
        return {kIn, uint16_t(n)};
      }
      case 0x08:   // GET_CONFIGURATION
        if (!in || length < 1) return stall;
        data[0] = stats.configuration;
        return {kIn, 1};
      case 0x09:   // SET_CONFIGURATION: endpoints now, the bulk transfers once the status stage is done
        if (in || length || value > 1 || !stats.address) return stall;
        unconfigure();
        if (value == 1 && !configure()) return stall;
        pendConfig = true;
        newConfig = uint8_t(value);
        return none;
      case 0x0a:   // GET_INTERFACE
        if (!in || length < 1 || index >= interfaces() || !stats.configuration) return stall;
        data[0] = netOn && index == netData() ? netAlt : 0;
        return {kIn, 1};
      case 0x0b:   // SET_INTERFACE: one alternate setting each, but the network data interface's two (endpoints now, as
                   // SET_CONFIGURATION; the transfers and the link once the status stage is done)
        if (in || length || index >= interfaces() || !stats.configuration) return stall;
        if (netOn && index == netData()) {
          if (value > 1 || !netSetAlt(uint8_t(value))) return stall;
          pendNetAlt = true;
          return none;
        }
        return value ? stall : none;
      default:
        return stall;
      }
    }
    if ((type & 0x7f) == 0x21 && netOn && index == netComm()) return netRequest(type, request, value, length, data);
    if ((type & 0x7f) == 0x21) {   // class, interface: CDC-ACM (PSTN) requests
      if (index >= 2 * portCount) return stall;
      const unsigned port = index / 2;
      switch (request) {
      case 0x20:   // SET_LINE_CODING
        if ((type & 0x80) || length < 7 || length > 64) return stall;
        outPort = int(port);
        outRequest = request;
        return {kOut, length};
      case 0x21:   // GET_LINE_CODING
        if (!(type & 0x80) || !length) return stall;
        for (unsigned i = 0; i < 7; ++i) data[i] = lineCoding[port][i];
        return {kIn, 7};
      case 0x22:   // SET_CONTROL_LINE_STATE
        if ((type & 0x80) || length) return stall;
        pendLinePort = int(port);
        pendLineValue = value;
        return none;
      case 0x23:   // SEND_BREAK
        if ((type & 0x80) || length) return stall;
        return none;
      default:
        return stall;
      }
    }
    return stall;
  }

  void dataOut(uint32_t got)
  {
    const volatile uint8_t *d = h->dma() + Layout::kEp0;
    if (outPort == int(kNetSlot)) {
      // SET_NTB_INPUT_SIZE: the largest NTB the host takes, within what the spec allows and this side's buffer.
      if (outRequest == ncm::SET_NTB_INPUT_SIZE && got >= 4) {
        const uint32_t size = uint32_t(d[0]) | uint32_t(d[1]) << 8 | uint32_t(d[2]) << 16 | uint32_t(d[3]) << 24;
        netStats.ntbInSize = size < ncm::kNtbMinIn ? ncm::kNtbMinIn : size > ncm::kNtbMax ? ncm::kNtbMax : size;
      }
      outPort = -1;   // SET_MAX_DATAGRAM_SIZE, SET_NET_ADDRESS, the multicast filters: taken, not applied
      return;
    }
    if (outRequest == 0x20 && outPort >= 0 && got >= 7)
      for (unsigned i = 0; i < 7; ++i) lineCoding[outPort][i] = d[i];
    outPort = -1;
  }

  void statusDone()
  {
    if (pendAddress) stats.address = newAddress;
    if (pendConfig) stats.configuration = newConfig;
    if (pendLinePort >= 0) {
      lineState[pendLinePort] = pendLineValue;
      h->lineState(unsigned(pendLinePort), pendLineValue);
    }
    if (pendNetAlt) netAltDone();
    pendAddress = pendConfig = pendNetAlt = false;
    pendLinePort = -1;
  }

  // ------------------------------------------------------------------------------------------------ the network function
  unsigned netComm() const { return 2 * portCount; }
  unsigned netData() const { return 2 * portCount + 1; }
  uint32_t netBitrate() const { return highSpeed ? 480000000u : 12000000u; }

  // The NCM requests (6.2) to the communications interface. NTB16 only, no CRC; the optional ones the host may still send are
  // taken and ignored rather than stalled.
  Reply netRequest(uint8_t type, uint8_t request, uint16_t value, uint16_t length, uint8_t *data)
  {
    using namespace ncm;
    const bool in = type & 0x80;
    const Reply stall = {kStall, 0}, none = {kNoData, 0};
    switch (request) {
    case GET_NTB_PARAMETERS:
      if (!in) return stall;
      return {kIn, uint16_t(ntbParameters(data, kNtbMax, kNtbMax))};
    case GET_NTB_FORMAT:
      if (!in) return stall;
      wr16(data, 0);   // NTB16
      return {kIn, 2};
    case SET_NTB_FORMAT:
      return in || length || value ? stall : none;
    case GET_NTB_INPUT_SIZE:
      if (!in) return stall;
      wr32(data, netStats.ntbInSize);
      return {kIn, 4};
    case GET_MAX_DATAGRAM_SIZE:
      if (!in) return stall;
      wr16(data, kNetMaxSegment);
      return {kIn, 2};
    case GET_CRC_MODE:
      if (!in) return stall;
      wr16(data, 0);
      return {kIn, 2};
    case SET_CRC_MODE:
      return in || length || value ? stall : none;
    case GET_NET_ADDRESS:
      if (!in) return stall;
      for (unsigned i = 0; i < 6; ++i) data[i] = kNetHostMac[i];
      return {kIn, 6};
    case SET_ETHERNET_PACKET_FILTER:
      if (in || length) return stall;
      netStats.filter = value;
      return none;
    case SET_NTB_INPUT_SIZE:
    case SET_MAX_DATAGRAM_SIZE:
    case SET_NET_ADDRESS:
    case SET_ETHERNET_MULTICAST_FILTERS:
      if (in || length > Layout::kEp0Bytes || (request == SET_NTB_INPUT_SIZE && length != 4 && length != 8)) return stall;
      if (!length) return none;
      outPort = int(kNetSlot);
      outRequest = request;
      return {kOut, length};
    default:
      return stall;   // encapsulated commands, statistics, power filters
    }
  }

  // SET_INTERFACE on the data interface, SETUP stage: alternate 1 has the bulk pair (configured afresh, so their sequence starts
  // over), alternate 0 none. Selecting alternate 0 resets the function (7.2): NTB input size, sequence number.
  bool netSetAlt(uint8_t alt)
  {
    using namespace dwc3;
    endXfer(kNetOutPhys);
    endXfer(kNetInPhys);
    ep[kNetOutPhys].configured = ep[kNetInPhys].configured = false;
    uint32_t enable = h->rd(DALEPENA) & ~(1u << kNetOutPhys | 1u << kNetInPhys);
    bool ok = true;
    if (alt == 1) {
      const unsigned mps = highSpeed ? 512 : 64;
      ok = configureEp(kNetOutPhys, EP_BULK, mps, DEPCFG_ACTION_INIT, 0) && configureEp(kNetInPhys, EP_BULK, mps, DEPCFG_ACTION_INIT, 0);
      if (ok) {
        ep[kNetOutPhys].configured = ep[kNetInPhys].configured = true;
        enable |= 1u << kNetOutPhys | 1u << kNetInPhys;
      }
    } else {
      netStats.ntbInSize = ncm::kNtbMax;
    }
    h->wr(DALEPENA, enable);
    netSeq = 0;
    netAltNew = alt;
    return ok;
  }

  // The status stage of SET_INTERFACE is done: the link follows the alternate setting; with alternate 1 the host is told the
  // speed, then that the network is connected (6.3, in that order).
  void netAltDone()
  {
    const bool was = netAlt == 1;
    netAlt = netAltNew;
    netStats.alt = netAlt;
    ++netStats.altChanges;
    if (netAlt == 1) {
      netNotifyPending = kNotifySpeed | kNotifyConnect;
      h->netLink(true, netBitrate());
    } else {
      netNotifyPending = 0;
      if (was) h->netLink(false, 0);
    }
  }

  // Configuration gone (SET_CONFIGURATION, bus reset, disconnect): the link is down and the function back at alternate 0.
  void netDown()
  {
    if (!netOn) return;
    const bool was = netAlt == 1;
    netAlt = netAltNew = 0;
    netStats.alt = 0;
    netNotifyPending = 0;
    netSeq = 0;
    netStats.ntbInSize = ncm::kNtbMax;
    if (was) h->netLink(false, 0);
  }

  void netNotify()
  {
    using namespace ncm;
    uint8_t *n = h->dma() + Layout::kNetNotify;
    uint32_t length = 8;
    n[0] = 0xa1;
    wr16(n + 4, netComm());
    if (netNotifyPending & kNotifySpeed) {   // ConnectionSpeedChange: downstream and upstream bit rates
      netNotifyPending &= uint8_t(~kNotifySpeed);
      n[1] = kSpeedChange;
      wr16(n + 2, 0);
      wr16(n + 6, 8);
      wr32(n + 8, netBitrate());
      wr32(n + 12, netBitrate());
      length = 16;
    } else {                                 // NetworkConnection: connected
      netNotifyPending &= uint8_t(~kNotifyConnect);
      n[1] = kNetworkConnection;
      wr16(n + 2, 1);
      wr16(n + 6, 0);
    }
    ++netStats.notifications;
    startXfer(kNetNotifyPhys, Layout::kNetNotify, length, dwc3::TRBCTL_NORMAL, false);
  }

  // While the host runs the link: the notifications, one NTB OUT queued whenever an NTB's worth of frames fits on this side, one
  // NTB IN built from the queued frames whenever there are some.
  void netPump()
  {
    if (!configured()) return;
    if (netNotifyPending && !ep[kNetNotifyPhys].busy && ep[kNetNotifyPhys].configured) netNotify();
    if (netAlt != 1) return;
    EpState &out = ep[kNetOutPhys], &in = ep[kNetInPhys];
    if (!out.busy && out.configured && h->netRxSpace() >= ncm::kNtbMax)
      startXfer(kNetOutPhys, Layout::kNetOut, ncm::kNtbMax, dwc3::TRBCTL_NORMAL, false);
    if (in.busy || !in.configured) return;
    uint8_t *const buffer = h->dma() + Layout::kNetIn;
    ncm::NtbWriter w;
    w.begin(buffer, netStats.ntbInSize);
    while (const uint32_t room = w.room()) {
      const unsigned n = h->netTxNext(buffer + w.offset(), room);
      if (!n) break;
      w.add(n);
      ++netStats.txFrames;
      netStats.txBytes += n;
    }
    const uint32_t total = w.finish(netSeq);
    if (!total) return;
    ++netSeq;
    ++netStats.txNtbs;
    // A block ending on a packet boundary gets a zero-length packet, unless it is exactly the host's NTB input size (3.8).
    const bool zlp = total % in.mps == 0 && total < netStats.ntbInSize;
    netStats.zlps += zlp;
    startXfer(kNetInPhys, Layout::kNetIn, total, dwc3::TRBCTL_NORMAL, zlp);
  }

  // An NTB from the host: every datagram the parser accepts goes to the Hal; a refused block is counted and its first few logged.
  void netReceived(uint32_t got)
  {
    if (!got) return;   // a zero-length packet after a block that ended on a packet boundary
    ++netStats.rxNtbs;
    const uint8_t *const b = h->dma() + Layout::kNetOut;
    const ncm::NtbResult r = ncm::parseNtb16(b, got, [&](uint32_t at, uint32_t n) {
      if (h->netRxFrame(b + at, n)) { ++netStats.rxFrames; netStats.rxBytes += n; }
      else ++netStats.rxDrops;
    });
    netStats.badDatagrams += r.bad;
    if (r.error) {
      netStats.lastError = r.error;
      if (++netStats.ntbErrors <= 5) h->note("net: NTB from the host refused (%s), %u bytes; %u so far", r.error, got, netStats.ntbErrors);
    }
    if (r.datagrams) h->netRxDone();
  }

  bool configure()
  {
    using namespace dwc3;
    const unsigned mps = highSpeed ? 512 : 64;
    uint32_t enable = 3;
    for (unsigned p = 0; p < portCount; ++p) {
      const unsigned notify = physOf(notifyEp(p), true), out = physOf(bulkEp(p), false), in = physOf(bulkEp(p), true);
      if (!configureEp(notify, EP_INTERRUPT, kNotifyMps, DEPCFG_ACTION_INIT, highSpeed ? kNotifyIntervalHs - 1 : 0) ||
          !configureEp(out, EP_BULK, mps, DEPCFG_ACTION_INIT, 0) || !configureEp(in, EP_BULK, mps, DEPCFG_ACTION_INIT, 0))
        return false;
      ep[notify].configured = ep[out].configured = ep[in].configured = true;
      enable |= 1u << notify | 1u << out | 1u << in;
    }
    // The network function: its notification endpoint; the data interface starts at alternate 0 (no bulk endpoints).
    if (netOn) {
      if (!configureEp(kNetNotifyPhys, EP_INTERRUPT, kNotifyMps, DEPCFG_ACTION_INIT, highSpeed ? kNotifyIntervalHs - 1 : 0)) return false;
      ep[kNetNotifyPhys].configured = true;
      enable |= 1u << kNetNotifyPhys;
    }
    h->wr(DALEPENA, enable);
    return true;
  }

  void unconfigure()
  {
    for (unsigned phys = 2; phys < kPhys; ++phys) {
      endXfer(phys);
      ep[phys].configured = false;
    }
    h->wr(dwc3::DALEPENA, 3);
    netDown();
  }

  void bulkComplete(unsigned phys)
  {
    volatile uint32_t *t = trb(phys);
    EpState &e = ep[phys];
    if (!e.busy || (t[3] & dwc3::TRB_HWO)) { ++stats.stale; return; }
    e.busy = false;
    if (phys >= kNetNotifyPhys) {   // the network function (notifications and the NTB IN need nothing more)
      if (phys == kNetOutPhys) {
        const uint32_t residual = dwc3::trbResidual(t[2]);
        netReceived(residual <= e.posted ? e.posted - residual : 0);
      }
      return;
    }
    if (phys < 3) return;
    const unsigned port = (phys - 3) / 4;
    if (port >= portCount || phys == physOf(notifyEp(port), true)) return;
    if (phys & 1) {
      ++stats.inTransfers;
      stats.inBytes += e.posted;
      return;
    }
    const uint32_t residual = dwc3::trbResidual(t[2]), got = residual <= e.posted ? e.posted - residual : 0;
    ++stats.outTransfers;
    stats.outBytes += got;
    if (got) h->rxData(port, h->dma() + Layout::portOff(port) + Layout::kOutOff, got);
  }
};

}  // namespace pianousb
