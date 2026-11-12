// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the PianoDwc3 gadget core (Dwc3Gadget.h + UsbDescriptors.h) against a model of the DWC3's command, TRB and
// event interface: enumeration as macOS does it, SET_ADDRESS timing, configuration, CDC-ACM class requests, bulk IN (with the
// zero-length packet rule) and OUT, stall of unknown requests, bus reset, full speed. Without the network function every
// descriptor is checked against the bytes of the ACM-only device as it was before the network function existed (golden FNV-1a
// sums); with it (piano-usb-net=1) the configuration is parsed as a host would, then the NCM requests, the alternate setting, the
// notifications, NTBs both ways (decoded by this file's own reading of NCM 1.0), the zero-length packet rule and the TX FIFO plan.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. usb_gadget_host.cpp -o /tmp/usb_gadget_host && /tmp/usb_gadget_host
#include "Dwc3Gadget.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
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

  FakeHal() { regs[GCTL] = 0x00102004; }   // the core's reset value on piano (C7, after USB30_PRIM_BCR): the device role
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
  // the network function's side
  std::deque<std::string> netTx;   // frames to the host
  std::vector<std::string> netRx;  // frames from the host
  unsigned netRoom = 1u << 20, netDone = 0, linkEvents = 0;
  bool linkUp = false;
  uint32_t linkBps = 0;
  unsigned netTxNext(uint8_t *dst, unsigned room)
  {
    if (netTx.empty() || netTx.front().size() > room) return 0;
    const unsigned n = unsigned(netTx.front().size());
    std::memcpy(dst, netTx.front().data(), n);
    netTx.pop_front();
    return n;
  }
  unsigned netRxSpace() { return netRoom; }
  bool netRxFrame(const uint8_t *src, unsigned len)
  {
    if (netRoom < len + 2) return false;
    netRx.emplace_back(reinterpret_cast<const char *>(src), len);
    return true;
  }
  void netRxDone() { ++netDone; }
  void netLink(bool up, uint32_t bps) { linkUp = up; linkBps = bps; ++linkEvents; }
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

// FNV-1a of the ACM-only descriptors as built before the network function existed (UsbDescriptors.h at 51124fe): ports 1..3,
// full/high speed, configuration (2) and other-speed configuration (7).
static unsigned fnv(const std::string &s)
{
  unsigned v = 2166136261u;
  for (unsigned char c : s) { v ^= c; v *= 16777619u; }
  return v;
}
static unsigned goldenConfig(unsigned P, bool hs, unsigned type)
{
  static const unsigned table[3][2][2] = {{{0x102fcc00, 0x22e84d29}, {0x6e24ea95, 0x05949a00}},
                                          {{0xa18751ad, 0x8b2578a4}, {0xf60f9b49, 0xa3202b10}},
                                          {{0xee790aed, 0x2391d440}, {0x990bd944, 0x35fe847d}}};
  return table[P - 1][hs][type == 7];
}
static std::string desc(unsigned (*f)(uint8_t *, unsigned, unsigned, unsigned), unsigned a, unsigned b)
{
  uint8_t buf[512];
  return std::string(reinterpret_cast<const char *>(buf), f(buf, sizeof buf, a, b));
}
static void goldenDescriptors()
{
  uint8_t buf[512];
  for (unsigned P = 1; P <= 3; ++P)
    for (bool hs : {false, true})
      for (unsigned type : {2u, 7u}) {
        const unsigned n = configDescriptor(buf, sizeof buf, uint8_t(type), hs, P, false);
        CHECK(n == 9 + 66 * P && fnv(std::string(reinterpret_cast<const char *>(buf), n)) == goldenConfig(P, hs, type));
        const unsigned m = configDescriptor(buf, sizeof buf, uint8_t(type), hs, P, true);
        CHECK(m == 9 + 66 * P + kNetFunctionBytes && buf[2] == uint8_t(m) && buf[3] == uint8_t(m >> 8) && buf[4] == 2 * P + 2);
      }
  unsigned n = deviceDescriptor(buf, sizeof buf, kProduct);
  CHECK(fnv(std::string(reinterpret_cast<const char *>(buf), n)) == 0x12889be8);
  n = qualifierDescriptor(buf, sizeof buf);
  CHECK(fnv(std::string(reinterpret_cast<const char *>(buf), n)) == 0x25c0b27a);
  static const unsigned strings[7] = {0xa84732ed, 0xa9f877ff, 0xd1237a4b, 0x7932f33a, 0xec16bc07, 0x7b480db0, 0xa66a021b};
  for (unsigned i = 0; i < 10; ++i) {
    const std::string acm = desc(stringDescriptor, i, kStringCountAcm), all = desc(stringDescriptor, i, kStringCount);
    if (i < 7) CHECK(fnv(acm) == strings[i] && acm == all);
    else CHECK(acm.empty() && (i < kStringCount) == !all.empty());
  }
}

// The host's end of NCM, written from the spec for this test: an NTB16 with the NDP right after the header (the layout hosts
// commonly send), and a strict reader of what the device sends (the alignment it advertises in GET_NTB_PARAMETERS).
static void put16(std::string &b, size_t at, unsigned v) { b[at] = char(v); b[at + 1] = char(v >> 8); }
static unsigned get16(const std::string &b, size_t at) { return uint8_t(b[at]) | uint8_t(b[at + 1]) << 8; }
static unsigned get32(const std::string &b, size_t at) { return get16(b, at) | get16(b, at + 2) << 16; }
static std::string hostNtb(const std::vector<std::string> &frames, unsigned sequence)
{
  std::string b(12 + 8 + 4 * (frames.size() + 1), '\0');
  std::vector<std::pair<unsigned, unsigned>> at;
  for (const std::string &f : frames) {
    while (b.size() % 4) b.push_back('\0');
    at.emplace_back(unsigned(b.size()), unsigned(f.size()));
    b += f;
  }
  b.replace(0, 4, "NCMH");
  put16(b, 4, 12);
  put16(b, 6, sequence);
  put16(b, 8, unsigned(b.size()));
  put16(b, 10, 12);
  b.replace(12, 4, "NCM0");
  put16(b, 16, unsigned(8 + 4 * (frames.size() + 1)));
  put16(b, 18, 0);
  for (size_t i = 0; i < at.size(); ++i) { put16(b, 20 + 4 * i, at[i].first); put16(b, 22 + 4 * i, at[i].second); }
  return b;
}
static bool deviceNtb(const std::string &b, std::vector<std::string> &frames, unsigned *sequence)
{
  if (b.size() < 12 || b.compare(0, 4, "NCMH") || get16(b, 4) != 12 || get16(b, 8) != b.size()) return false;
  *sequence = get16(b, 6);
  const unsigned ndp = get16(b, 10);
  if (ndp % 4 || ndp + 16 > b.size() || b.compare(ndp, 4, "NCM0") || get16(b, ndp + 6) != 0) return false;
  const unsigned length = get16(b, ndp + 4);
  if (length < 16 || length % 4 || ndp + length > b.size()) return false;
  for (unsigned e = ndp + 8; e + 4 <= ndp + length; e += 4) {
    const unsigned at = get16(b, e), n = get16(b, e + 2);
    if (!at && !n) return true;
    if (at % 4 || at < 12 || at + n > ndp) return false;
    frames.push_back(b.substr(at, n));
  }
  return false;
}
static std::string bytes(unsigned n, unsigned seed)
{
  std::string s(n, '\0');
  for (unsigned i = 0; i < n; ++i) s[i] = char(seed + 13 * i);
  return s;
}
static std::string le32(unsigned v) { return std::string{char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; }

// The network function on a configured device of P ports, as a Mac would drive it: the configuration read as a host parses it,
// the NCM requests, SET_INTERFACE(data, 1), the notifications, NTBs both ways, flow control, alternate 0 again.
static void netScenario(FakeHal &h, G &g, unsigned P)
{
  const uint16_t comm = uint16_t(2 * P), data = uint16_t(2 * P + 1);
  // The configuration as a host parses it: the IAD, the communications interface and its functional descriptors, the data
  // interface's two alternates.
  {
    const std::string c = controlIn(h, g, setupBytes(0x80, 6, 0x0200, 0, 0xffff));
    unsigned at = 0, iface = 0xff, alt = 0, seen = 0, ethernetMac = 0, segment = 0;
    bool iad = false, header = false, unionOk = false, ncmOk = false, notify = false, alt0 = false, alt1 = false, bulkIn = false, bulkOut = false;
    while (at + 2 <= c.size()) {
      const unsigned len = uint8_t(c[at]), type = uint8_t(c[at + 1]);
      const auto u = [&](unsigned i) { return unsigned(uint8_t(c[at + i])); };
      if (type == 11 && u(2) == comm) iad = u(3) == 2 && u(4) == 0x02 && u(5) == 0x0d && u(6) == 0 && u(7) == kStrNet;
      if (type == 4) {
        iface = u(2);
        alt = u(3);
        if (iface == comm) CHECK(alt == 0 && u(4) == 1 && u(5) == 0x02 && u(6) == 0x0d && u(7) == 0);
        if (iface == data) {
          CHECK(u(5) == 0x0a && u(6) == 0 && u(7) == 0x01);
          if (alt == 0) alt0 = u(4) == 0;
          if (alt == 1) alt1 = u(4) == 2;
        }
      }
      if (type == 0x24 && iface == comm) {
        ++seen;
        if (u(2) == 0x00) header = len == 5 && (u(3) | u(4) << 8) == 0x0110;
        if (u(2) == 0x06) unionOk = len == 5 && u(3) == comm && u(4) == data;
        if (u(2) == 0x0f) { CHECK(len == 13); ethernetMac = u(3); segment = u(8) | u(9) << 8; }
        if (u(2) == 0x1a) ncmOk = len == 6 && (u(3) | u(4) << 8) == 0x0100 && u(5) == 0x01;
      }
      if (type == 5 && iface == comm) notify = u(2) == 0x80 + kNetNotifyEp && u(3) == 3 && (u(4) | u(5) << 8) == kNotifyMps && u(6) == 9;
      if (type == 5 && iface == data) {
        CHECK(alt == 1);
        if (u(2) == kNetBulkEp) bulkOut = u(3) == 2 && (u(4) | u(5) << 8) == 512;
        if (u(2) == 0x80 + kNetBulkEp) bulkIn = u(3) == 2 && (u(4) | u(5) << 8) == 512;
      }
      at += len;
    }
    CHECK(iad && header && unionOk && ncmOk && notify && alt0 && alt1 && bulkIn && bulkOut && seen == 4);
    CHECK(ethernetMac == kStrNetMac && segment == 1514);
  }
  // NCM requests.
  std::string p = controlIn(h, g, setupBytes(0xa1, ncm::GET_NTB_PARAMETERS, 0, comm, 28));
  CHECK(p.size() == 28 && get16(p, 0) == 28 && get16(p, 2) == 1 && get32(p, 4) == ncm::kNtbMax && get16(p, 8) == 4 && get16(p, 12) == 4 &&
        get32(p, 16) == ncm::kNtbMax && get16(p, 20) == 4 && get16(p, 24) == 4);
  CHECK(get16(controlIn(h, g, setupBytes(0xa1, ncm::GET_NTB_FORMAT, 0, comm, 2)), 0) == 0);
  controlNoData(h, g, setupBytes(0x21, ncm::SET_NTB_FORMAT, 0, comm, 0));
  CHECK(stalls(h, g, setupBytes(0x21, ncm::SET_NTB_FORMAT, 1, comm, 0)));   // no NTB32
  CHECK(get32(controlIn(h, g, setupBytes(0xa1, ncm::GET_NTB_INPUT_SIZE, 0, comm, 4)), 0) == ncm::kNtbMax);
  const struct { unsigned asked, kept; } sizes[] = {{4096, 4096}, {100, ncm::kNtbMinIn}, {65536, ncm::kNtbMax}, {8192, 8192}};
  for (const auto &s : sizes) {
    controlOut(h, g, setupBytes(0x21, ncm::SET_NTB_INPUT_SIZE, 0, comm, 4), le32(s.asked));
    CHECK(get32(controlIn(h, g, setupBytes(0xa1, ncm::GET_NTB_INPUT_SIZE, 0, comm, 4)), 0) == s.kept);
  }
  controlOut(h, g, setupBytes(0x21, ncm::SET_NTB_INPUT_SIZE, 0, comm, 8), le32(ncm::kNtbMax) + le32(0));   // with wNtbInMaxDatagrams
  CHECK(g.netStats.ntbInSize == ncm::kNtbMax);
  CHECK(stalls(h, g, setupBytes(0x21, ncm::SET_NTB_INPUT_SIZE, 0, comm, 2)));
  controlNoData(h, g, setupBytes(0x21, ncm::SET_ETHERNET_PACKET_FILTER, 0x000e, comm, 0));
  CHECK(g.netStats.filter == 0x000e);
  controlNoData(h, g, setupBytes(0x21, ncm::SET_CRC_MODE, 0, comm, 0));
  CHECK(stalls(h, g, setupBytes(0x21, ncm::SET_CRC_MODE, 1, comm, 0)));
  CHECK(get16(controlIn(h, g, setupBytes(0xa1, ncm::GET_MAX_DATAGRAM_SIZE, 0, comm, 2)), 0) == 1514);
  CHECK(controlIn(h, g, setupBytes(0xa1, ncm::GET_NET_ADDRESS, 0, comm, 6)) == std::string(reinterpret_cast<const char *>(kNetHostMac), 6));
  controlOut(h, g, setupBytes(0x21, ncm::SET_ETHERNET_MULTICAST_FILTERS, 2, comm, 12), std::string(12, '\x01'));
  CHECK(stalls(h, g, setupBytes(0xa1, ncm::GET_ETHERNET_STATISTIC, 1, comm, 4)));
  CHECK(stalls(h, g, setupBytes(0x21, 0x22, 3, data, 0)));   // no class requests on the data interface
  CHECK(controlIn(h, g, setupBytes(0x81, 0x0a, 0, data, 1)) == std::string(1, '\0'));
  CHECK(stalls(h, g, setupBytes(0x01, 0x0b, 2, data, 0)));   // no alternate 2
  CHECK(stalls(h, g, setupBytes(0x01, 0x0b, 1, comm, 0)));   // the communications interface has one
  CHECK(stalls(h, g, setupBytes(0x02, 1, 0, 0x88, 0)));       // HALT on a bulk endpoint of alternate 0: not there yet

  // SET_INTERFACE(data, 1): the bulk pair configured in the SETUP stage, the link and the transfers after the status stage.
  const unsigned links = h.linkEvents;
  sendSetup(h, g, setupBytes(0x01, 0x0b, 1, data, 0));
  const FakeHal::Cmd *o = h.last(CMD_SETEPCONFIG, kNetOutPhys), *i = h.last(CMD_SETEPCONFIG, kNetInPhys);
  CHECK(o && ((o->p0 >> 1) & 3) == EP_BULK && ((o->p0 >> 3) & 0x7ff) == 512 && ((o->p0 >> 17) & 0x1f) == 0 && ((o->p1 >> 25) & 0x1f) == kNetOutPhys);
  CHECK(i && ((i->p0 >> 1) & 3) == EP_BULK && ((i->p0 >> 17) & 0x1f) == kNetBulkEp && ((i->p1 >> 25) & 0x1f) == kNetInPhys);
  CHECK((h.regs[DALEPENA] & (1u << kNetOutPhys | 1u << kNetInPhys)) == (1u << kNetOutPhys | 1u << kNetInPhys));
  CHECK(!h.linkUp && h.linkEvents == links && !h.eps[kNetOutPhys].active);
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  h.complete(1);
  step(g);
  CHECK(h.linkUp && h.linkBps == 480000000 && g.netLinkUp() && g.netStats.alt == 1);
  CHECK(controlIn(h, g, setupBytes(0x81, 0x0a, 0, data, 1)) == std::string(1, '\1'));
  // The notifications: ConnectionSpeedChange (16 bytes), then NetworkConnection (8), each one transfer, then nothing.
  CHECK(h.eps[kNetNotifyPhys].active);
  const std::string n1 = h.complete(kNetNotifyPhys);
  step(g);
  CHECK(n1.size() == 16 && uint8_t(n1[0]) == 0xa1 && uint8_t(n1[1]) == 0x2a && get16(n1, 2) == 0 && get16(n1, 4) == comm &&
        get16(n1, 6) == 8 && get32(n1, 8) == 480000000 && get32(n1, 12) == 480000000);
  CHECK(h.eps[kNetNotifyPhys].active);
  const std::string n2 = h.complete(kNetNotifyPhys);
  step(g);
  CHECK(n2.size() == 8 && uint8_t(n2[0]) == 0xa1 && n2[1] == 0 && get16(n2, 2) == 1 && get16(n2, 4) == comm && get16(n2, 6) == 0);
  CHECK(!h.eps[kNetNotifyPhys].active && g.netStats.notifications == 2);

  // A host clearing the halt of the bulk pair with the OUT transfer queued: END TRANSFER first, then CLEARSTALL, then queued again.
  CHECK(h.eps[kNetOutPhys].active);
  {
    const auto lastIndex = [&](uint32_t command, unsigned phys) {
      for (size_t k = h.cmds.size(); k-- > 0;) if (h.cmds[k].command == command && h.cmds[k].phys == phys) return long(k);
      return -1L;
    };
    const unsigned ends = h.count(CMD_ENDTRANSFER, kNetOutPhys);
    controlNoData(h, g, setupBytes(0x02, 1, 0, kNetBulkEp, 0));
    CHECK(h.count(CMD_ENDTRANSFER, kNetOutPhys) == ends + 1 && lastIndex(CMD_ENDTRANSFER, kNetOutPhys) < lastIndex(CMD_CLEARSTALL, kNetOutPhys));
    CHECK(h.eps[kNetOutPhys].active);
  }
  // From the host: an NTB of three frames; the OUT transfer is a whole NTB (16 KiB) and is queued again.
  CHECK(h.eps[kNetOutPhys].active && trbResidual(h.trb(kNetOutPhys)[2]) == ncm::kNtbMax);
  const std::vector<std::string> in = {bytes(98, 1), bytes(1514, 2), bytes(42, 3)};
  h.complete(kNetOutPhys, hostNtb(in, 0));
  step(g);
  CHECK(h.netRx == in && h.netDone == 1 && g.netStats.rxFrames == 3 && g.netStats.rxNtbs == 1 && h.eps[kNetOutPhys].active);
  h.complete(kNetOutPhys, std::string("NCMX") + std::string(60, '\0'));   // not an NTB: counted, nothing delivered
  step(g);
  CHECK(h.netRx.size() == 3 && g.netStats.ntbErrors == 1 && h.eps[kNetOutPhys].active);
  h.complete(kNetOutPhys, std::string());   // a zero-length packet: nothing
  step(g);
  CHECK(g.netStats.rxNtbs == 2 && g.netStats.ntbErrors == 1 && h.eps[kNetOutPhys].active);
  h.netRoom = 1000;   // no room for a whole NTB: no transfer queued, then the frame that does not fit is dropped
  h.complete(kNetOutPhys, hostNtb({bytes(1200, 4)}, 1));
  step(g);
  CHECK(!h.eps[kNetOutPhys].active && g.netStats.rxDrops == 1 && h.netRx.size() == 3);
  h.netRoom = 1u << 20;
  step(g);
  CHECK(h.eps[kNetOutPhys].active);

  // To the host: queued frames go out as one NTB16, decoded here as a host would; the next NTB has the next sequence number.
  const std::vector<std::string> out = {bytes(60, 5), bytes(1514, 6), bytes(100, 7)};
  for (const std::string &f : out) h.netTx.push_back(f);
  step(g);
  CHECK(h.eps[kNetInPhys].active);
  bool zlp = true;
  std::string block = h.complete(kNetInPhys, std::string(), &zlp);
  step(g);
  std::vector<std::string> frames;
  unsigned sequence = 99;
  CHECK(deviceNtb(block, frames, &sequence) && frames == out && sequence == 0 && !zlp && h.netTx.empty());
  // A block of exactly 512 bytes (one 484-byte frame) ends on a packet boundary: a zero-length packet follows.
  h.netTx.push_back(bytes(484, 8));
  step(g);
  block = h.complete(kNetInPhys, std::string(), &zlp);
  step(g);
  frames.clear();
  CHECK(block.size() == 512 && zlp && deviceNtb(block, frames, &sequence) && sequence == 1 && g.netStats.zlps == 1);
  // The host's NTB input size bounds the blocks: 2048 takes 1000 + 1016 bytes exactly (no zero-length packet: it is the size the
  // host reads), and two full frames go in two blocks.
  controlOut(h, g, setupBytes(0x21, ncm::SET_NTB_INPUT_SIZE, 0, comm, 4), le32(2048));
  h.netTx.push_back(bytes(1000, 9));
  h.netTx.push_back(bytes(1016, 10));
  step(g);
  block = h.complete(kNetInPhys, std::string(), &zlp);
  step(g);
  frames.clear();
  CHECK(block.size() == 2048 && !zlp && deviceNtb(block, frames, &sequence) && frames.size() == 2 && sequence == 2);
  h.netTx.push_back(bytes(1514, 11));
  h.netTx.push_back(bytes(1514, 12));
  step(g);
  block = h.complete(kNetInPhys, std::string(), &zlp);
  step(g);
  frames.clear();
  CHECK(deviceNtb(block, frames, &sequence) && frames.size() == 1 && frames[0] == bytes(1514, 11) && sequence == 3 && block.size() <= 2048);
  CHECK(h.eps[kNetInPhys].active);
  block = h.complete(kNetInPhys, std::string(), &zlp);
  step(g);
  frames.clear();
  CHECK(deviceNtb(block, frames, &sequence) && frames.size() == 1 && frames[0] == bytes(1514, 12) && sequence == 4 && h.netTx.empty());
  CHECK(g.netStats.txNtbs == 5 && g.netStats.txFrames == 8);

  // Alternate 0: the bulk pair goes, the link goes down after the status stage, the function is reset (input size, sequence).
  h.netTx.push_back(bytes(70, 13));
  step(g);
  CHECK(h.eps[kNetInPhys].active);
  controlNoData(h, g, setupBytes(0x01, 0x0b, 0, data, 0));
  CHECK(!h.linkUp && !h.eps[kNetInPhys].active && !h.eps[kNetOutPhys].active && !(h.regs[DALEPENA] & (1u << kNetOutPhys | 1u << kNetInPhys)));
  CHECK(g.netStats.ntbInSize == ncm::kNtbMax && g.netStats.alt == 0);
  CHECK(controlIn(h, g, setupBytes(0x81, 0x0a, 0, data, 1)) == std::string(1, '\0'));
  // And back to 1: link up again, both notifications again, the sequence starts at 0.
  controlNoData(h, g, setupBytes(0x01, 0x0b, 1, data, 0));
  CHECK(h.linkUp && h.eps[kNetNotifyPhys].active);
  h.complete(kNetNotifyPhys);
  step(g);
  h.complete(kNetNotifyPhys);
  step(g);
  h.netTx.push_back(bytes(70, 14));
  step(g);
  block = h.complete(kNetInPhys, std::string(), &zlp);
  step(g);
  frames.clear();
  CHECK(deviceNtb(block, frames, &sequence) && sequence == 0 && frames.size() == 1 && g.netStats.notifications == 4);
  // Alternate 0 while a notification is still unread: it is withdrawn (a stale "connected"), nothing more is sent; back to 1.
  controlNoData(h, g, setupBytes(0x01, 0x0b, 0, data, 0));
  controlNoData(h, g, setupBytes(0x01, 0x0b, 1, data, 0));
  CHECK(h.eps[kNetNotifyPhys].active && h.linkUp);
  controlNoData(h, g, setupBytes(0x01, 0x0b, 0, data, 0));
  CHECK(!h.eps[kNetNotifyPhys].active && !h.linkUp);
  step(g);
  CHECK(!h.eps[kNetNotifyPhys].active);
  controlNoData(h, g, setupBytes(0x01, 0x0b, 1, data, 0));
  CHECK(h.linkUp && h.eps[kNetNotifyPhys].active);
  // The ACM ports kept working throughout: port 0's bulk OUT is still queued.
  CHECK(h.eps[physOf(bulkEp(0), false)].active);
}

// The whole scenario for a device of P ports (2: piano-usb-hid off, the device as it has always been; 3: with the input port),
// without and with the network function.
static void scenario(unsigned P, bool net, bool quiet)
{
  FakeHal h;
  h.quiet = quiet;
  G g{};
  g.bind(&h, P, kProduct, 1000, net);
  h.regs[DSTS] = DSTS_DEVCTRLHLT;
  h.regs[GTXFIFOSIZ0] = 0x43;   // EP0's FIFO as the firmware leaves it (T1)
  TxFifoPlan plan;
  if (net) {
    const uint8_t packets[16] = {0, 1, 4, 1, 4, uint8_t(P == 3), uint8_t(P == 3 ? 4 : 0), 1, 12};
    plan = planTxFifos(packets, 16, h.regs[GTXFIFOSIZ0], 3462, 8, true);
    CHECK(!plan.error);
    g.setTxFifoPlan(plan);
  }
  uint32_t waited = 1;
  CHECK(g.softReset(true, &waited) && waited == 0);
  CHECK(g.init());
  CHECK(h.regs[GTXFIFOSIZ0] == 0x43);
  for (unsigned n = 1; n < 16; ++n) {   // the plan's FIFOs only, and nothing at all without the network function
    const bool written = h.regs.count(GTXFIFOSIZ0 + 4 * n) != 0;
    CHECK(written == (net && plan.value[n] != 0));
    if (written) CHECK(h.regs[GTXFIFOSIZ0 + 4 * n] == plan.value[n]);
  }
  CHECK((h.regs[GCTL] & GCTL_PRTCAPDIR_MASK) == GCTL_PRTCAP_DEVICE);
  CHECK((h.regs[DCFG] & DCFG_DEVSPD_MASK) == DCFG_SPEED_HS && !(h.regs[DCFG] & DCFG_LPM_CAP));
  CHECK(h.regs[GEVNTADRLO] == uint32_t(h.base) && h.regs[GEVNTADRHI] == uint32_t(h.base >> 32));
  CHECK(h.regs[GEVNTSIZ] == (GEVNTSIZ_INTMASK | 4096));
  CHECK(h.count(CMD_DEPSTARTCFG, 0) == 1 && h.count(CMD_SETEPCONFIG, 0) == 1 && h.count(CMD_SETEPCONFIG, 1) == 1);
  for (unsigned phys = 0; phys < kPhys; ++phys)
    CHECK(h.count(CMD_SETTRANSFRESOURCE, phys) == (phys < 2 + 4 * P || (net && phys >= kNetNotifyPhys) ? 1u : 0u));
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
  CHECK(fnv(dev) == 0x12889be8);   // the same device descriptor with and without the network function
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
  CHECK(total == 9 + P * 66 + (net ? kNetFunctionBytes : 0));
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
  if (!net) CHECK(fnv(config) == goldenConfig(P, true, 2));
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
    // The network function adds an IAD, three interface descriptors (communications; data alternates 0 and 1), three endpoints.
    const unsigned extra = net ? 1 : 0;
    CHECK(at == config.size() && interfaces == 2 * P + 3 * extra && iads == P + extra && endpoints == 3 * P + 3 * extra);
    CHECK(addresses.size() == 3 * P + 3 * extra && addresses[0] == 0x81 && addresses[1] == 0x02 && addresses[2] == 0x82 &&
          addresses[3] == 0x83 && addresses[4] == 0x04 && addresses[5] == 0x84);
    if (P == 3) CHECK(addresses[6] == 0x85 && addresses[7] == 0x06 && addresses[8] == 0x86);
    if (net) CHECK(addresses[3 * P] == 0x87 && addresses[3 * P + 1] == 0x08 && addresses[3 * P + 2] == 0x88);
    // The input port's functions are named "piano input" (string 6).
    if (P == 3) CHECK(uint8_t(config[9 + 2 * 66 + 7]) == 6 && uint8_t(config[9 + 2 * 66 + 8 + 8]) == 6);
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
  if (!net) CHECK(fnv(other) == goldenConfig(P, false, 7));
  if (!net) CHECK(stalls(h, g, setupBytes(0x80, 6, 0x0307, 0x0409, 255)));   // the ACM-only device has no string 7
  if (net) {   // the function's name and the Mac's MAC address (iMACAddress: 12 hex digits, UTF-16)
    const std::string name = controlIn(h, g, setupBytes(0x80, 6, 0x0307, 0x0409, 255));
    CHECK(name.size() == 2 + 2 * std::strlen("piano network") && name[2] == 'p');
    const std::string mac = controlIn(h, g, setupBytes(0x80, 6, 0x0308, 0x0409, 255));
    CHECK(mac.size() == 26 && uint8_t(mac[0]) == 26 && uint8_t(mac[1]) == 3);
    std::string hex;
    for (size_t i = 2; i + 1 < mac.size(); i += 2) { CHECK(mac[i + 1] == 0); hex += mac[i]; }
    for (unsigned i = 0; i < 6; ++i) CHECK(std::stoul(hex.substr(2 * i, 2), nullptr, 16) == kNetHostMac[i]);
    CHECK(std::memcmp(kNetHostMac, kNetDeviceMac, 6) != 0 && (kNetHostMac[0] & 3) == 2 && (kNetDeviceMac[0] & 3) == 2);
  }
  CHECK(stalls(h, g, setupBytes(0x80, 6, 0x0f00, 0, 5)));        // BOS: a USB 2.0 device has none
  CHECK(stalls(h, g, setupBytes(0x80, 6, 0x0309, 0x0409, 255)));  // no string 9
  CHECK(stalls(h, g, setupBytes(0xc0, 0x55, 0, 0, 4)));           // vendor request
  std::string status = controlIn(h, g, setupBytes(0x80, 0, 0, 0, 2));
  CHECK(status.size() == 2 && status[0] == 1);
  std::string cfgv = controlIn(h, g, setupBytes(0x80, 8, 0, 0, 1));
  CHECK(cfgv.size() == 1 && cfgv[0] == 0);

  // SET_CONFIGURATION(1): endpoints configured before the status stage, the OUT transfers queued after it.
  sendSetup(h, g, setupBytes(0x00, 9, 1, 0, 0));
  for (unsigned p = 0; p < P; ++p) {
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
    if (p >= P) { CHECK(!h.eps[out].active && !(h.regs[DALEPENA] & 1u << out)); continue; }
    CHECK(h.eps[out].active && h.trbType(out) == TRBCTL_NORMAL && trbResidual(h.trb(out)[2]) == Layout::kOutBytes);
  }
  if (net) {   // the network function's notification endpoint (TX FIFO 7); its data interface is at alternate 0: no bulk pair
    const FakeHal::Cmd *n = h.last(CMD_SETEPCONFIG, kNetNotifyPhys);
    CHECK(n && ((n->p0 >> 1) & 3) == EP_INTERRUPT && ((n->p0 >> 17) & 0x1f) == kNetNotifyEp && ((n->p0 >> 3) & 0x7ff) == kNotifyMps);
    CHECK((h.regs[DALEPENA] & 1u << kNetNotifyPhys) && !(h.regs[DALEPENA] & (1u << kNetOutPhys | 1u << kNetInPhys)));
    CHECK(!h.last(CMD_SETEPCONFIG, kNetOutPhys) && !h.eps[kNetOutPhys].active && !h.eps[kNetNotifyPhys].active && !h.linkUp);
  } else {
    for (unsigned phys = kNetNotifyPhys; phys <= kNetInPhys; ++phys) CHECK(!h.last(CMD_SETEPCONFIG, phys) && !(h.regs[DALEPENA] & 1u << phys));
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
  CHECK(stalls(h, g, setupBytes(0x21, 0x22, 3, 2 * P, 0)));   // no such interface
  if (P == 3) {   // the input port: DTR, and a command line arriving on its bulk OUT
    controlNoData(h, g, setupBytes(0x21, 0x22, 1, 4, 0));
    CHECK(h.lines[2] == 1);
    const unsigned out2 = physOf(bulkEp(2), false);
    CHECK(h.eps[out2].active);
    h.complete(out2, "K 0 4\n");
    step(g);
    CHECK(h.rx[2] == "K 0 4\n" && h.eps[out2].active);
  }

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
  if (!net) CHECK(stalls(h, g, setupBytes(0x02, 1, 0, 0x88, 0)));   // no endpoint 8 without the network function

  if (net) netScenario(h, g, P);

  // Bus reset while configured: transfers ended, lines dropped, address and configuration cleared.
  h.tx[0] = "pending";
  step(g);
  CHECK(h.eps[in0].active);
  h.devEvent(DEVT_RESET);
  step(g);
  CHECK(!g.configured() && g.stats.address == 0 && h.lines[0] == 0 && h.lines[1] == 0);
  CHECK(!h.eps[in0].active && !h.eps[out0].active && h.regs[DALEPENA] == 3 && ((h.regs[DCFG] >> 3) & 0x7f) == 0);
  CHECK(h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  if (net) CHECK(!h.linkUp && !h.eps[kNetOutPhys].active && !h.eps[kNetInPhys].active && g.netStats.alt == 0);

  // Full speed: 64-byte bulk packets.
  h.regs[DSTS] = (h.regs[DSTS] & ~7u) | SPEED_FS_48;
  h.devEvent(DEVT_CONNECT_DONE);
  step(g);
  CHECK(g.stats.speed == SPEED_FS_48);
  controlNoData(h, g, setupBytes(0x00, 5, 9, 0, 0));
  std::string fs = controlIn(h, g, setupBytes(0x80, 6, 0x0200, 0, 0xffff));
  CHECK(fs.size() == total && uint8_t(fs[9 + 8 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 4]) == 64);   // first bulk endpoint's wMaxPacketSize
  if (!net) CHECK(fnv(fs) == goldenConfig(P, false, 2));
  sendSetup(h, g, setupBytes(0x00, 9, 1, 0, 0));
  CHECK(((h.last(CMD_SETEPCONFIG, out0)->p0 >> 3) & 0x7ff) == 64);
  h.epEvent(1, EPT_XFER_NOT_READY, EP0_NOTREADY_STATUS);
  step(g);
  h.complete(1);
  step(g);
  CHECK(g.configured());
  if (net) {   // full speed: 64-byte bulk packets for the network function too, 12 Mb/s reported
    CHECK(uint8_t(fs[total - 3]) == 64 && uint8_t(fs[total - 5]) == 0x88);
    controlNoData(h, g, setupBytes(0x01, 0x0b, 1, uint16_t(2 * P + 1), 0));
    CHECK(((h.last(CMD_SETEPCONFIG, kNetInPhys)->p0 >> 3) & 0x7ff) == 64 && h.linkUp && h.linkBps == 12000000);
  }
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
    CHECK(c1.size() == 9 + 66 && uint8_t(c1[4]) == 2 && fnv(c1) == goldenConfig(1, true, 2));
  }
  CHECK(g.stats.cmdErrors == 0 && g.stats.cmdTimeouts == 0 && g.stats.startErrors == 0);
  std::printf("%u ports%s: %u events, %u setups, %u stalls, %llu bytes in, %llu out", P, net ? " + network" : "", g.stats.events,
              g.stats.setups, g.stats.stalls, (unsigned long long)g.stats.inBytes, (unsigned long long)g.stats.outBytes);
  if (net)
    std::printf("; net: %u NTBs (%u frames, %llu bytes) to the host, %u NTBs (%u frames, %llu bytes) from it, %u NTB errors, %u bad "
                "datagrams, %u drops, %u notifications, %u zero-length packets",
                g.netStats.txNtbs, g.netStats.txFrames, (unsigned long long)g.netStats.txBytes, g.netStats.rxNtbs, g.netStats.rxFrames,
                (unsigned long long)g.netStats.rxBytes, g.netStats.ntbErrors, g.netStats.badDatagrams, g.netStats.rxDrops,
                g.netStats.notifications, g.netStats.zlps);
  std::printf("\n");
}

// A system sleep without a host kept (UsbSleep.h quiesce): sleepDisconnect halts the core (RUN_STOP off), ends every transfer and
// forgets the configuration; sleepConnect after the wake runs again with SETUP armed and the counters kept; the host's new
// enumeration configures the device as at the first attach.
static void sleepCycle(bool quiet)
{
  FakeHal h;
  h.quiet = quiet;
  G g{};
  g.bind(&h, 2, kProduct, 0, false);
  h.regs[DSTS] = DSTS_DEVCTRLHLT;
  CHECK(g.init() && g.run(10));
  auto attach = [&](uint16_t address) {
    h.devEvent(DEVT_RESET);
    h.regs[DSTS] = (h.regs[DSTS] & ~7u) | SPEED_HS;
    h.devEvent(DEVT_CONNECT_DONE);
    step(g);
    controlNoData(h, g, setupBytes(0x00, 5, address, 0, 0));
    controlNoData(h, g, setupBytes(0x00, 9, 1, 0, 0));
  };
  attach(5);
  CHECK(g.configured() && g.stats.address == 5);
  const unsigned resets = g.stats.resets, setups = g.stats.setups;
  bool busy = false;
  for (unsigned phys = 2; phys < kPhys; ++phys) busy = busy || h.eps[phys].active;
  CHECK(busy);   // the ports' bulk transfers queued while configured
  h.tx[0] = std::string(300, 'x');   // console bytes in a bulk IN the host has not taken yet
  g.pump();
  CHECK(g.inBusy(0) && g.takeInUnconfirmed(0) == 300 && g.takeInUnconfirmed(0) == 300);   // in flight: counted while it is
  CHECK(g.sleepDisconnect());
  CHECK(g.takeInUnconfirmed(0) == 300 && g.takeInUnconfirmed(0) == 0);   // ended unfinished by the disconnect: counted once
  CHECK(!(h.regs[DCTL] & DCTL_RUN_STOP) && (h.regs[DSTS] & DSTS_DEVCTRLHLT));
  CHECK(!g.configured() && g.stats.address == 0 && ((h.regs[DCFG] >> 3) & 0x7f) == 0);
  for (unsigned phys = 0; phys < kPhys; ++phys) CHECK(!h.eps[phys].active);
  g.pump();   // nothing queued while disconnected
  for (unsigned phys = 0; phys < kPhys; ++phys) CHECK(!h.eps[phys].active);
  const unsigned startcfg = h.count(CMD_DEPSTARTCFG, 0);
  CHECK(g.sleepConnect());
  CHECK((h.regs[DCTL] & DCTL_RUN_STOP) && !(h.regs[DSTS] & DSTS_DEVCTRLHLT));
  CHECK(h.count(CMD_DEPSTARTCFG, 0) == startcfg + 1 && h.eps[0].active && h.trbType(0) == TRBCTL_CONTROL_SETUP);
  CHECK(g.stats.resets == resets && g.stats.setups == setups);   // the counters kept across the sleep
  attach(6);
  CHECK(g.configured() && g.stats.address == 6 && g.stats.resets == resets + 1);
  CHECK(g.stats.cmdErrors == 0 && g.stats.cmdTimeouts == 0 && g.stats.startErrors == 0);
  std::printf("sleep cycle: disconnected, connected again, enumerated afresh\n");
}

// The core outside the device role (C6: the host role at the takeover): every entry that would touch a device register refuses, and
// none is touched (no endpoint command, DCTL and DCFG as they were, the role left alone).
static void hostRoleRefused()
{
  for (uint32_t gctl : {0x00101005u, 0x00103005u, 0x00100005u}) {
    FakeHal h;
    h.regs[GCTL] = gctl;
    h.regs[DCTL] = 0x10f00000;
    h.regs[DCFG] = 0x0048080c;
    G g;
    g.bind(&h, 3, 0x0001);
    uint32_t waited = 7;
    CHECK(!g.halt(10) && !g.softReset(true, &waited) && waited == 0 && !g.init() && !g.initDevice() && !g.run(10));
    CHECK(h.cmds.empty() && h.regs[DCTL] == 0x10f00000 && h.regs[DCFG] == 0x0048080c && h.regs[GCTL] == gctl);
  }
}

int main(int argc, char **argv)
{
  (void)argv;
  hostRoleRefused();
  sleepCycle(argc < 2);
  goldenDescriptors();
  scenario(2, false, argc < 2);
  scenario(3, false, argc < 2);
  scenario(2, true, argc < 2);
  scenario(3, true, argc < 2);
  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
