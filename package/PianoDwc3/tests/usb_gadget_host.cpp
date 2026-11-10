// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the PianoDwc3 gadget core (Dwc3Gadget.h + UsbDescriptors.h) against a model of the DWC3's command, TRB and
// event interface: enumeration as macOS does it, SET_ADDRESS timing, configuration, CDC-ACM class requests, bulk IN (with the
// zero-length packet rule) and OUT, stall of unknown requests, bus reset, full speed.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. usb_gadget_host.cpp -o /tmp/usb_gadget_host && /tmp/usb_gadget_host
#include "Dwc3Gadget.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace pianousb;
using namespace pianousb::dwc3;

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct FakeHal {
  std::map<uint32_t, uint32_t> regs;
  std::vector<uint8_t> mem = std::vector<uint8_t>(Layout::kBytes);
  const uint64_t base = 0x8c1230000ull;
  struct Cmd { unsigned phys; uint32_t command, p0, p1, p2; };
  std::vector<Cmd> cmds;
  struct EpModel { bool active, stalled; uint64_t trb; };
  EpModel eps[32] = {};
  uint32_t evWrite = 0;
  std::string tx[kPorts], rx[kPorts];
  unsigned rxRoom = 1u << 20;
  uint16_t lines[kPorts] = {};
  bool quiet = true;

  uint32_t rd(uint32_t off) { return regs[off]; }
  void wr(uint32_t off, uint32_t v)
  {
    if (off >= 0xc800 && off < 0xca00 && (off & 0xf) == 0xc) { command((off - 0xc800) / 16, v); return; }
    if (off == GEVNTCOUNT) { assert(v <= regs[off]); regs[off] -= v; return; }
    if (off == DCTL) {
      v &= ~DCTL_CSFTRST;
      if (v & DCTL_RUN_STOP) regs[DSTS] &= ~DSTS_DEVCTRLHLT; else regs[DSTS] |= DSTS_DEVCTRLHLT;
    }
    regs[off] = v;
  }
  void command(unsigned phys, uint32_t v)
  {
    const uint32_t b = depcmdBase(phys);
    Cmd c{phys, v & 0xf, regs[b + 8], regs[b + 4], regs[b + 0]};
    cmds.push_back(c);
    uint32_t result = v & ~DEPCMD_CMDACT;
    switch (c.command) {
    case CMD_STARTTRANSFER:
      assert(!eps[phys].active);
      eps[phys].active = true;
      eps[phys].trb = uint64_t(c.p0) << 32 | c.p1;
      result = (result & 0xff80ffffu) | (phys + 1) << 16;
      break;
    case CMD_ENDTRANSFER: eps[phys].active = false; break;
    case CMD_SETSTALL: eps[phys].stalled = true; break;
    case CMD_CLEARSTALL: eps[phys].stalled = false; break;
    default: break;
    }
    regs[b + 12] = result;
  }
  uint64_t now = 0;
  void barrier() {}
  void udelay(unsigned us) { now += us; }
  uint64_t nowUs() { return now; }
  uint8_t *dma() { return mem.data(); }
  uint64_t dmaPhys() { return base; }
  unsigned txFill(unsigned port, uint8_t *dst, unsigned cap)
  {
    const unsigned n = tx[port].size() < cap ? unsigned(tx[port].size()) : cap;
    std::memcpy(dst, tx[port].data(), n);
    tx[port].erase(0, n);
    return n;
  }
  unsigned rxSpace(unsigned) { return rxRoom; }
  void rxData(unsigned port, const uint8_t *src, unsigned len) { rx[port].append(reinterpret_cast<const char *>(src), len); }
  void lineState(unsigned port, uint16_t state) { lines[port] = state; }
  void note(const char *fmt, ...)
  {
    if (quiet) return;
    va_list ap;
    va_start(ap, fmt);
    std::printf("  note: ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
  }

  // device side of the model
  uint8_t *at(uint64_t pa) { assert(pa >= base && pa + 1 <= base + Layout::kBytes); return &mem[pa - base]; }
  uint32_t *trb(unsigned phys, unsigned n = 0) { return reinterpret_cast<uint32_t *>(at(eps[phys].trb + 16 * n)); }
  void event(uint32_t e)
  {
    std::memcpy(&mem[Layout::kEvents + evWrite], &e, 4);
    evWrite = (evWrite + 4) % Layout::kEventBytes;
    regs[GEVNTCOUNT] += 4;
  }
  void devEvent(uint32_t type, uint32_t info = 0) { event(1u | type << 8 | info << 16); }
  void epEvent(unsigned phys, uint32_t type, uint32_t status = 0) { event(phys << 1 | type << 6 | status << 12); }
  unsigned trbType(unsigned phys, unsigned n = 0) { return (trb(phys, n)[3] >> 4) & 0x3f; }
  // The host's side of one transfer: consumes the TRB (and a chained zero-length one), returns IN data or takes OUT data.
  std::string complete(unsigned phys, const std::string &out = std::string(), bool *zlp = nullptr)
  {
    assert(eps[phys].active);
    uint32_t *t = trb(phys);
    assert(t[3] & TRB_HWO);
    const uint32_t len = trbResidual(t[2]);
    std::string data;
    if (phys & 1) data.assign(reinterpret_cast<const char *>(at(uint64_t(t[1]) << 32 | t[0])), len);
    else {
      assert(out.size() <= len);
      std::memcpy(at(uint64_t(t[1]) << 32 | t[0]), out.data(), out.size());
    }
    const bool chained = t[3] & TRB_CHN;
    if (zlp) *zlp = chained;
    if (chained) {
      uint32_t *z = trb(phys, 1);
      assert((z[3] & TRB_HWO) && (z[3] & TRB_LST) && trbResidual(z[2]) == 0);
      z[3] &= ~TRB_HWO;
    }
    t[2] = (phys & 1) ? 0 : uint32_t(len - out.size());
    t[3] &= ~TRB_HWO;
    eps[phys].active = false;
    epEvent(phys, EPT_XFER_COMPLETE);
    return data;
  }
  unsigned count(uint32_t command, unsigned phys) const
  {
    unsigned n = 0;
    for (const Cmd &c : cmds) n += c.command == command && c.phys == phys;
    return n;
  }
  const Cmd *last(uint32_t command, unsigned phys) const
  {
    for (auto i = cmds.rbegin(); i != cmds.rend(); ++i) if (i->command == command && i->phys == phys) return &*i;
    return nullptr;
  }
};

using G = Gadget<FakeHal>;
static void step(G &g) { while (g.poll()) {} g.pump(); g.tick(); }

static std::string setupBytes(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length)
{
  const char s[8] = {char(type), char(request), char(value), char(value >> 8), char(index), char(index >> 8), char(length), char(length >> 8)};
  return std::string(s, 8);
}
static void sendSetup(FakeHal &h, G &g, const std::string &s)
{
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  h.complete(0, s);
  step(g);
}
static std::string controlIn(FakeHal &h, G &g, const std::string &s, bool *zlp = nullptr)
{
  sendSetup(h, g, s);
  CHECK(h.eps[1].active && h.trbType(1) == TRBCTL_CONTROL_DATA);
  if (!h.eps[1].active) return std::string();
  const std::string data = h.complete(1, std::string(), zlp);
  step(g);
  h.epEvent(0, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_STATUS3);
  h.complete(0);
  step(g);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  return data;
}
static void controlNoData(FakeHal &h, G &g, const std::string &s)
{
  sendSetup(h, g, s);
  CHECK(!h.eps[1].active);
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  CHECK(h.eps[1].active && h.trbType(1) == TRBCTL_CONTROL_STATUS2);
  h.complete(1);
  step(g);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
}
static void controlOut(FakeHal &h, G &g, const std::string &s, const std::string &data)
{
  sendSetup(h, g, s);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_DATA);
  CHECK(trbResidual(h.trb(0)[2]) % 64 == 0);
  h.complete(0, data);
  step(g);
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  CHECK(h.eps[1].active && h.trbType(1) == TRBCTL_CONTROL_STATUS3);
  h.complete(1);
  step(g);
}
static bool stalls(FakeHal &h, G &g, const std::string &s)
{
  const unsigned before = h.count(CMD_SETSTALL, 0);
  sendSetup(h, g, s);
  return h.count(CMD_SETSTALL, 0) == before + 1 && h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP;
}

int main(int argc, char **argv)
{
  FakeHal h;
  h.quiet = argc < 2;
  (void)argv;
  G g{};
  g.bind(&h, kPorts, kProduct, 1000);
  h.regs[DSTS] = DSTS_DEVCTRLHLT;
  uint32_t waited = 1;
  CHECK(g.softReset(true, &waited) && waited == 0);
  CHECK(g.init());
  CHECK((h.regs[GCTL] & GCTL_PRTCAPDIR_MASK) == GCTL_PRTCAP_DEVICE);
  CHECK((h.regs[DCFG] & DCFG_DEVSPD_MASK) == DCFG_SPEED_HS && !(h.regs[DCFG] & DCFG_LPM_CAP));
  CHECK(h.regs[GEVNTADRLO] == uint32_t(h.base) && h.regs[GEVNTADRHI] == uint32_t(h.base >> 32));
  CHECK(h.regs[GEVNTSIZ] == (GEVNTSIZ_INTMASK | 4096));
  CHECK(h.count(CMD_DEPSTARTCFG, 0) == 1 && h.count(CMD_SETEPCONFIG, 0) == 1 && h.count(CMD_SETEPCONFIG, 1) == 1);
  for (unsigned phys = 0; phys < kPhys; ++phys) CHECK(h.count(CMD_SETTRANSFRESOURCE, phys) == 1);
  CHECK(h.regs[DALEPENA] == 3);
  CHECK(g.run(10));
  CHECK((h.regs[DCTL] & DCTL_RUN_STOP) && !(h.regs[DSTS] & DSTS_DEVCTRLHLT));
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP && trbResidual(h.trb(0)[2]) == 8);

  // A host attaches at high speed.
  h.devEvent(DEVT_RESET);
  h.regs[DSTS] = (h.regs[DSTS] & ~7u) | SPEED_HS;
  h.devEvent(DEVT_CONNECT_DONE);
  step(g);
  CHECK(g.stats.resets == 1 && g.stats.connects == 1 && g.stats.speed == SPEED_HS);
  const FakeHal::Cmd *cfg0 = h.last(CMD_SETEPCONFIG, 0);
  CHECK(cfg0 && (cfg0->p0 & DEPCFG_ACTION_MODIFY) && ((cfg0->p0 >> 3) & 0x7ff) == 64);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);

  std::string dev = controlIn(h, g, setupBytes(0x80, 6, 0x0100, 0, 64));
  CHECK(dev.size() == 18 && uint8_t(dev[0]) == 18 && uint8_t(dev[1]) == 1 && uint8_t(dev[4]) == 0xef && uint8_t(dev[7]) == 64);
  CHECK(uint8_t(dev[8]) == 0x09 && uint8_t(dev[9]) == 0x12 && uint8_t(dev[10]) == 0x01 && uint8_t(dev[11]) == 0x00);

  // SET_ADDRESS: DCFG holds the address before the status stage, the software state after it.
  sendSetup(h, g, setupBytes(0x00, 5, 7, 0, 0));
  CHECK(((h.regs[DCFG] >> 3) & 0x7f) == 7 && g.stats.address == 0);
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  CHECK(h.eps[1].active && h.trbType(1) == TRBCTL_CONTROL_STATUS2);
  h.complete(1);
  step(g);
  CHECK(g.stats.address == 7);

  // The data stage carries min(wLength, descriptor) bytes: 9 of 141 here (the T2 log printed the descriptor's length).
  sendSetup(h, g, setupBytes(0x80, 6, 0x0200, 0, 9));
  CHECK(h.eps[1].active && trbResidual(h.trb(1)[2]) == 9 && !(h.trb(1)[3] & TRB_CHN));
  std::string head = h.complete(1);
  step(g);
  h.epEvent(0, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  h.complete(0);
  step(g);
  CHECK(head.size() == 9);
  const unsigned total = uint8_t(head[2]) | uint8_t(head[3]) << 8;
  CHECK(total == 9 + kPorts * 66);
  sendSetup(h, g, setupBytes(0x80, 6, 0x0302, 0x0409, 2));
  CHECK(h.eps[1].active && trbResidual(h.trb(1)[2]) == 2);
  h.complete(1);
  step(g);
  h.epEvent(0, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  h.complete(0);
  step(g);
  // The full configuration exactly as macOS asks for it (wLength 141: three packets, no zero-length packet).
  std::string config = controlIn(h, g, setupBytes(0x80, 6, 0x0200, 0, uint16_t(total)));
  CHECK(config.size() == total);
  // The same with the status stage's not-ready event ahead of the data stage's completion: still one status, SETUP again.
  sendSetup(h, g, setupBytes(0x80, 6, 0x0200, 0, uint16_t(total)));
  CHECK(h.eps[1].active && trbResidual(h.trb(1)[2]) == total);
  h.trb(1)[2] = 0;
  h.trb(1)[3] &= ~TRB_HWO;
  h.eps[1].active = false;
  h.epEvent(0, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  h.epEvent(1, EPT_XFER_COMPLETE);
  step(g);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_STATUS3);
  h.complete(0);
  step(g);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  // EP0 watchdog: a status stage that never comes; after 1 s the transfer is ended and SETUP armed again.
  sendSetup(h, g, setupBytes(0x80, 6, 0x0100, 0, 18));
  h.complete(1);
  step(g);
  CHECK(!h.eps[0].active && g.stats.ep0Timeouts == 0);
  h.now += 1500000;
  step(g);
  CHECK(g.stats.ep0Timeouts == 1 && h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  config = controlIn(h, g, setupBytes(0x80, 6, 0x0200, 0, 0xffff));
  CHECK(config.size() == total);
  {
    unsigned at = 0, interfaces = 0, endpoints = 0, iads = 0;
    std::vector<unsigned> addresses;
    while (at + 2 <= config.size()) {
      const unsigned len = uint8_t(config[at]), type = uint8_t(config[at + 1]);
      CHECK(len >= 2 && at + len <= config.size());
      if (len < 2) break;
      if (type == 4) ++interfaces;
      if (type == 11) ++iads;
      if (type == 5) {
        ++endpoints;
        addresses.push_back(uint8_t(config[at + 2]));
        const unsigned mps = uint8_t(config[at + 4]) | uint8_t(config[at + 5]) << 8;
        CHECK(uint8_t(config[at + 3]) == 3 ? mps == kNotifyMps : mps == 512);
      }
      at += len;
    }
    CHECK(at == config.size() && interfaces == 2 * kPorts && iads == kPorts && endpoints == 3 * kPorts);
    CHECK(addresses.size() == 6 && addresses[0] == 0x81 && addresses[1] == 0x02 && addresses[2] == 0x82 && addresses[3] == 0x83 &&
          addresses[4] == 0x04 && addresses[5] == 0x84);
  }
  std::string lang = controlIn(h, g, setupBytes(0x80, 6, 0x0300, 0, 255));
  CHECK(lang.size() == 4 && uint8_t(lang[2]) == 0x09 && uint8_t(lang[3]) == 0x04);
  std::string product = controlIn(h, g, setupBytes(0x80, 6, 0x0302, 0x0409, 255));
  CHECK(product.size() == 2 + 2 * std::strlen("piano macOS debug") && product[2] == 'p' && product[3] == 0);
  std::string truncated = controlIn(h, g, setupBytes(0x80, 6, 0x0302, 0x0409, 2));
  CHECK(truncated.size() == 2);
  std::string qualifier = controlIn(h, g, setupBytes(0x80, 6, 0x0600, 0, 10));
  CHECK(qualifier.size() == 10 && uint8_t(qualifier[1]) == 6);
  std::string other = controlIn(h, g, setupBytes(0x80, 6, 0x0700, 0, 0xffff));
  CHECK(other.size() == total && uint8_t(other[1]) == 7);
  CHECK(stalls(h, g, setupBytes(0x80, 6, 0x0f00, 0, 5)));        // BOS: a USB 2.0 device has none
  CHECK(stalls(h, g, setupBytes(0x80, 6, 0x0309, 0x0409, 255)));  // no string 9
  CHECK(stalls(h, g, setupBytes(0xc0, 0x55, 0, 0, 4)));           // vendor request
  std::string status = controlIn(h, g, setupBytes(0x80, 0, 0, 0, 2));
  CHECK(status.size() == 2 && status[0] == 1);
  std::string cfgv = controlIn(h, g, setupBytes(0x80, 8, 0, 0, 1));
  CHECK(cfgv.size() == 1 && cfgv[0] == 0);

  // SET_CONFIGURATION(1): endpoints configured before the status stage, the OUT transfers queued after it.
  sendSetup(h, g, setupBytes(0x00, 9, 1, 0, 0));
  for (unsigned p = 0; p < kPorts; ++p) {
    const unsigned notify = physOf(notifyEp(p), true), out = physOf(bulkEp(p), false), in = physOf(bulkEp(p), true);
    const FakeHal::Cmd *n = h.last(CMD_SETEPCONFIG, notify), *o = h.last(CMD_SETEPCONFIG, out), *i = h.last(CMD_SETEPCONFIG, in);
    CHECK(n && ((n->p0 >> 1) & 3) == EP_INTERRUPT && ((n->p0 >> 17) & 0x1f) == notifyEp(p) && ((n->p1 >> 25) & 0x1f) == notify);
    CHECK(o && ((o->p0 >> 1) & 3) == EP_BULK && ((o->p0 >> 3) & 0x7ff) == 512 && ((o->p0 >> 17) & 0x1f) == 0 && ((o->p1 >> 25) & 0x1f) == out);
    CHECK(i && ((i->p0 >> 1) & 3) == EP_BULK && ((i->p0 >> 17) & 0x1f) == bulkEp(p) && ((i->p1 >> 25) & 0x1f) == in);
    CHECK(h.regs[DALEPENA] & (1u << notify | 1u << out | 1u << in));
    CHECK(!h.eps[out].active);
  }
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  h.complete(1);
  step(g);
  CHECK(g.configured());
  for (unsigned p = 0; p < kPorts; ++p) {
    const unsigned out = physOf(bulkEp(p), false);
    CHECK(h.eps[out].active && h.trbType(out) == TRBCTL_NORMAL && trbResidual(h.trb(out)[2]) == Layout::kOutBytes);
  }

  // CDC-ACM: line coding round trip, then DTR/RTS reaches the hook after the status stage.
  const std::string coding("\x00\x10\x0e\x00\x00\x00\x08", 7);   // 921600 8N1
  controlOut(h, g, setupBytes(0x21, 0x20, 0, 0, 7), coding);
  CHECK(std::memcmp(g.lineCoding[0], coding.data(), 7) == 0);
  std::string got = controlIn(h, g, setupBytes(0xa1, 0x21, 0, 0, 7));
  CHECK(got == coding);
  controlNoData(h, g, setupBytes(0x21, 0x22, 3, 2, 0));   // port 1
  CHECK(h.lines[1] == 3 && h.lines[0] == 0);
  controlNoData(h, g, setupBytes(0x21, 0x22, 3, 0, 0));
  CHECK(h.lines[0] == 3);
  CHECK(stalls(h, g, setupBytes(0x21, 0x22, 3, 2 * kPorts, 0)));   // no such interface

  // Bulk IN: a short transfer, then one ending on a packet boundary (chained zero-length packet).
  h.tx[0] = "hello";
  step(g);
  const unsigned in0 = physOf(bulkEp(0), true), out0 = physOf(bulkEp(0), false);
  CHECK(h.eps[in0].active);
  bool zlp = true;
  CHECK(h.complete(in0, std::string(), &zlp) == "hello" && !zlp);
  step(g);
  h.tx[0] = std::string(1024, 'x');
  step(g);
  CHECK(h.complete(in0, std::string(), &zlp) == std::string(1024, 'x') && zlp);
  step(g);
  CHECK(!h.eps[in0].active && g.stats.inBytes == 5 + 1024);
  // Bulk OUT: data reaches the port, the next transfer is queued again; no room, no transfer.
  h.complete(out0, "help\r");
  step(g);
  CHECK(h.rx[0] == "help\r" && h.eps[out0].active);
  h.rxRoom = 100;
  h.complete(out0, "x");
  step(g);
  CHECK(!h.eps[out0].active);
  h.rxRoom = 1u << 20;
  step(g);
  CHECK(h.eps[out0].active);

  // CLEAR_FEATURE(ENDPOINT_HALT) on the bulk IN endpoint.
  controlNoData(h, g, setupBytes(0x02, 1, 0, 0x82, 0));
  CHECK(h.count(CMD_CLEARSTALL, in0) == 1);
  CHECK(stalls(h, g, setupBytes(0x02, 1, 0, 0x8f, 0)));

  // Bus reset while configured: transfers ended, lines dropped, address and configuration cleared.
  h.tx[0] = "pending";
  step(g);
  CHECK(h.eps[in0].active);
  h.devEvent(DEVT_RESET);
  step(g);
  CHECK(!g.configured() && g.stats.address == 0 && h.lines[0] == 0 && h.lines[1] == 0);
  CHECK(!h.eps[in0].active && !h.eps[out0].active && h.regs[DALEPENA] == 3 && ((h.regs[DCFG] >> 3) & 0x7f) == 0);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);

  // Full speed: 64-byte bulk packets.
  h.regs[DSTS] = (h.regs[DSTS] & ~7u) | SPEED_FS_48;
  h.devEvent(DEVT_CONNECT_DONE);
  step(g);
  CHECK(g.stats.speed == SPEED_FS_48);
  controlNoData(h, g, setupBytes(0x00, 5, 9, 0, 0));
  std::string fs = controlIn(h, g, setupBytes(0x80, 6, 0x0200, 0, 0xffff));
  CHECK(fs.size() == total && uint8_t(fs[9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 4]) == 64);   // first bulk endpoint's wMaxPacketSize
  sendSetup(h, g, setupBytes(0x00, 9, 1, 0, 0));
  CHECK(((h.last(CMD_SETEPCONFIG, out0)->p0 >> 3) & 0x7ff) == 64);
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  h.complete(1);
  step(g);
  CHECK(g.configured());
  // SET_CONFIGURATION before SET_ADDRESS is refused.
  h.devEvent(DEVT_RESET);
  step(g);
  CHECK(stalls(h, g, setupBytes(0x00, 9, 1, 0, 0)));
  // A status stage the host starts in the wrong direction is stalled and SETUP re-armed.
  sendSetup(h, g, setupBytes(0x00, 5, 3, 0, 0));
  const unsigned stallsBefore = g.stats.stalls;
  h.epEvent(0, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  CHECK(g.stats.stalls == stallsBefore + 1 && h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  // Not configured and no request for 10 s: one soft disconnect/connect (counters kept), not repeated at once.
  {
    const unsigned setupsBefore = g.stats.setups, startcfg = h.count(CMD_DEPSTARTCFG, 0);
    h.now += 11000000;
    step(g);
    CHECK(g.stats.reconnects == 1 && g.stats.setups == setupsBefore && h.count(CMD_DEPSTARTCFG, 0) == startcfg + 1);
    CHECK((h.regs[DCTL] & DCTL_RUN_STOP) && h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP && g.stats.address == 0);
    step(g);
    CHECK(g.stats.reconnects == 1);
    h.devEvent(DEVT_RESET);
    h.devEvent(DEVT_CONNECT_DONE);
    step(g);
    controlNoData(h, g, setupBytes(0x00, 5, 4, 0, 0));
    CHECK(g.stats.address == 4);
  }
  // One port only (a core with too few IN endpoints).
  {
    FakeHal h1;
    G g1{};
    g1.bind(&h1, 1, kProduct);
    h1.regs[DSTS] = DSTS_DEVCTRLHLT;
    CHECK(g1.init() && g1.run(1));
    h1.devEvent(DEVT_RESET);
    h1.devEvent(DEVT_CONNECT_DONE);
    step(g1);
    controlNoData(h1, g1, setupBytes(0x00, 5, 1, 0, 0));
    std::string c1 = controlIn(h1, g1, setupBytes(0x80, 6, 0x0200, 0, 0xffff));
    CHECK(c1.size() == 9 + 66 && uint8_t(c1[4]) == 2);
  }
  CHECK(g.stats.cmdErrors == 0 && g.stats.cmdTimeouts == 0 && g.stats.startErrors == 0);
  std::printf("%s: %d failure(s); %u events, %u setups, %u stalls, %llu bytes in, %llu out\n", failures ? "FAILED" : "ok", failures,
              g.stats.events, g.stats.setups, g.stats.stalls, (unsigned long long)g.stats.inBytes, (unsigned long long)g.stats.outBytes);
  return failures ? 1 : 0;
}
