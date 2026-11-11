// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoTouch's portable parts against models (not hardware):
//   - Nt36532.h against a model of the NT36532E's SPI protocol as Nt36532.h describes it (page select, auto-incrementing writes and
//     reads with the dummy byte, the engine and bootloader resets, the trim ID on one of the three hw-register tables, ENB_CASC,
//     auto copy, the hardware CRC check at BOOT_RDY over what reached its SRAM, the reset and calibration states, host commands,
//     poll info, firmware info, THP frames with their checksum), over a whole-transfer bus (the GENI engine's shape) and over
//     Se2Bus.h's SPI by hand driving a bit-level model of the same chip;
//   - the image parser on synthetic images (with and without a cascaded second header) and, when given, on the real images: every
//     partition downloaded into the model must pass the model's CRC check (byte-exact SRAM contents); nothing of an image is printed;
//   - TouchMatrix.h on synthetic frames (type lock, median reference, line noise, centroid, Y flip, release) and the tip debounce;
//   - TouchHid.h's descriptor (parsed: one 5-byte input report) and the report scaling; Se2Bus.h's clock decoding.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoDwc3 touch_host.cpp -o /tmp/touch_host && /tmp/touch_host [IMAGE.bin ...]
#include "Nt36532.h"
#include "TouchMatrix.h"
#include "TouchHid.h"
#include "Se2Bus.h"
#include "TouchPace.h"
#ifdef PIANO_TOUCH_SYNTHETIC_HEADER
#include PIANO_TOUCH_SYNTHETIC_HEADER   // tests/synthetic-calibration.json through tooling/k27/gen-touch-calibration.py
#endif
#ifdef PIANO_TOUCH_UNIT_HEADER
#include PIANO_TOUCH_UNIT_HEADER        // the unit's private calibration (out/ only): properties checked, nothing printed
#endif
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <vector>

using namespace pianotouch;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- the chip model
struct ChipModel {
  std::map<uint32_t, uint8_t> mem;
  uint32_t page = 0, base = 0;
  std::vector<uint8_t> in;
  bool reading = false;
  enum class St { rom, bootloader, running } st = St::rom;
  unsigned answerTable = 0;                     // the hw-register table whose reset makes the trim ID readable
  uint8_t id[6] = {0x0e, 0x01, 0x02, 0x32, 0x65, 0x03};
  uint8_t enbCasc = 0x00;                       // bit 0 clear: cascade
  bool crcOn = false, takeCommands = true;
  int autoCopyLeft = 0, resetReads = 0;
  uint32_t payload = 5192;
  uint16_t counter = 0;
  unsigned bootChecks = 0, bootFails = 0, engResets = 0, bootloaderResets = 0, transfers = 0;
  std::vector<std::pair<uint8_t, uint16_t>> hostCmds;
  uint32_t eventBuf() const { return kEventCascadeE; }

  uint8_t get(uint32_t a) const { auto i = mem.find(a); return i == mem.end() ? 0 : i->second; }
  uint32_t get24(uint32_t a) const { return get(a) | get(a + 1) << 8 | get(a + 2) << 16; }
  uint32_t get32(uint32_t a) const { return get24(a) | uint32_t(get(a + 3)) << 24; }
  bool isTrim(uint32_t a) const { return a >= kHwRegs[answerTable].trim && a < kHwRegs[answerTable].trim + 6; }
  uint8_t readByte(uint32_t a)
  {
    if (isTrim(a)) return st == St::bootloader ? id[a - kHwRegs[answerTable].trim] : 0;
    if (a == kEnbCasc) return enbCasc;
    if (a == kTxAutoCopyEn) {
      if (autoCopyLeft > 0) { --autoCopyLeft; return 0x56; }
      return 0;
    }
    if (a == eventBuf() + kEvResetComplete && st == St::running) return ++resetReads < 3 ? 0xa0 : 0xa3;
    return get(a);
  }
  // A read transfer: MISO byte i (0 and 1: the offset and the dummy) from the offset byte alone.
  uint8_t out(size_t i) { return reading && i >= 2 ? readByte(base + uint32_t(i - 2)) : 0; }
  void begin() { in.clear(); reading = false; ++transfers; }
  void take(uint8_t b)
  {
    if (in.empty()) {
      reading = !(b & 0x80);
      base = page + (b & 0x7f);
    }
    in.push_back(b);
  }
  void end()
  {
    if (in.empty() || reading) return;
    if (in[0] == 0xff) {
      if (in.size() >= 3) page = uint32_t(in[1]) << 15 | uint32_t(in[2]) << 7;
      return;
    }
    const uint32_t a = base;
    const size_t n = in.size() - 1;
    for (size_t k = 0; k < n; ++k)
      if (!isTrim(a + uint32_t(k))) mem[a + uint32_t(k)] = in[1 + k];
    const uint8_t v = in[1];
    if (a == kEngReset && v == 0x5a) { st = St::rom; ++engResets; }
    for (unsigned t = 0; t < 3; ++t)
      if (a == kHwRegs[t].swrst && v == 0x69) {
        ++bootloaderResets;
        st = t == answerTable || st == St::bootloader ? St::bootloader : St::rom;
        crcOn = false;
        resetReads = 0;
        mem[eventBuf() + kEvResetComplete] = 0;
      }
    if (a == kTxAutoCopyEn && v == 0x56) autoCopyLeft = 3;
    if (a == eventBuf() + kEvHostCmd && v == 0xae) crcOn = true;
    if (a == eventBuf() + kEvHostCmd && v == 0xbf && n >= 5) {
      hostCmds.push_back({in[2], uint16_t(in[4] | in[5] << 8)});
      if (takeCommands) mem[eventBuf() + kEvHostCmd] = 0;   // taken
    }
    if (a == kBootRdy && v == 1) boot();
  }
  uint32_t crcAt(uint32_t des, uint32_t len)
  {
    std::vector<uint8_t> b((len + 4) / 4 * 4);
    for (size_t i = 0; i < b.size(); ++i) b[i] = get(des + uint32_t(i));
    return crc32Words(b.data(), len, uint32_t(b.size()));
  }
  void boot()
  {
    ++bootChecks;
    const bool ilm = crcAt(get24(kIlmDes), get24(kIlmLength)) == get32(kIlmGolden);
    const bool dlm = crcAt(get24(kDlmDes), get24(kDlmLength)) == get32(kDlmGolden);
    if (st != St::bootloader || !crcOn || !ilm || !dlm) { ++bootFails; return; }
    st = St::running;
    resetReads = 0;
    mem[kPollInfoCascadeE + 2] = uint8_t(payload);
    mem[kPollInfoCascadeE + 3] = uint8_t(payload >> 8);
    const uint32_t info = eventBuf() + kEvFwInfo;
    mem[info] = 0x21;
    mem[info + 1] = 0xde;
    mem[info + 2] = 40;
    mem[info + 3] = 60;
  }
  // A THP frame into the event buffer: transport block bytes 0..255, the packet from byte 256. extra(r, c) on a 1000 baseline.
  template <class Extra>
  void produce(uint8_t type, Extra extra, bool badSum = false, uint8_t transport = 0x01)
  {
    const uint32_t e = eventBuf();
    for (uint32_t i = 0; i < 256; ++i) mem[e + i] = uint8_t(transport + (i & 0x0f) * (transport == 0x01));
    std::vector<uint8_t> p(payload, 0);
    p[0] = 0xa5;
    put16(&p[2], counter);
    p[24] = 2;
    put16(&p[28], counter);
    p[48] = 60;
    p[49] = 40;
    p[56] = type;
    for (uint32_t r = 0; r < 40; ++r)
      for (uint32_t c = 0; c < 60; ++c) put16(&p[64 + (r * 60 + c) * 2], uint16_t(1000 + int((r * 7 + c * 13 + counter * 5) % 9) - 4 + extra(r, c)));
    const uint32_t words = (payload - 20) / 4;
    uint32_t s = 0;
    for (uint32_t i = 0; i < words * 2; ++i) s += le16(&p[20 + i * 2]);
    const uint16_t sum = uint16_t((~s + 1) ^ (badSum ? 1 : 0));
    put16(&p[4], sum);
    put32(&p[8], words);
    put16(&p[12], uint16_t(~sum));
    put32(&p[16], ~words);
    for (uint32_t i = 0; i < payload; ++i) mem[e + 256 + i] = p[i];
    ++counter;
  }
};

// The whole-transfer bus (the GENI engine's shape): one transfer = one chip-select window.
struct ByteBus {
  ChipModel *m;
  uint32_t maxLen = 0, sleptMs = 0;
  static int xfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len)
  {
    ByteBus *b = static_cast<ByteBus *>(ctx);
    if (len > 65536) return -22;   // the DMA buffer
    if (len > b->maxLen) b->maxLen = len;
    b->m->begin();
    for (uint32_t i = 0; i < len; ++i) {
      const uint8_t o = b->m->out(i);
      if (rx) rx[i] = o;
      b->m->take(tx[i]);
    }
    b->m->end();
    return 0;
  }
  static void sleep(void *ctx, unsigned ms) { static_cast<ByteBus *>(ctx)->sleptMs += ms; }
};

// A bit-level mode-0 slave around the same model, for bitSpiTransfer.
struct BitSlave {
  ChipModel *m;
  bool cs = true, clk = false, mosi = false;
  uint8_t shiftIn = 0, cur = 0;
  unsigned bits = 0;
  size_t index = 0;
  uint64_t edges = 0;
  bool misoLevel() const { return !cs && (cur & 0x80); }
  void setCs(bool high)
  {
    if (cs && !high) { m->begin(); index = 0; bits = 0; cur = m->out(0); }
    if (!cs && high) m->end();
    cs = high;
  }
  void setClk(bool high)
  {
    if (!cs && !clk && high) {   // rising: sample
      shiftIn = uint8_t(shiftIn << 1 | (mosi ? 1 : 0));
      ++edges;
    }
    if (!cs && clk && !high) {   // falling: next bit out
      cur = uint8_t(cur << 1);
      if (++bits == 8) {
        m->take(shiftIn);
        bits = 0;
        ++index;
        cur = m->out(index);
      }
    }
    clk = high;
  }
};
BitSlave *gSlave;
const PinIo kSlaveIo = {nullptr, [](void *, bool h) { gSlave->mosi = h; }, [](void *, bool h) { gSlave->setClk(h); },
                        [](void *, bool h) { gSlave->setCs(h); }, [](void *) { return gSlave->misoLevel(); }, [](void *, unsigned) {}};
int bitXfer(void *, const uint8_t *tx, uint8_t *rx, uint32_t len)
{
  bitSpiTransfer(kSlaveIo, tx, rx, len);
  return 0;
}

// ------------------------------------------------------------------------------------------------------------- images
std::vector<uint8_t> syntheticImage(bool cascade2nd, uint32_t seed)
{
  std::vector<uint8_t> d(0x10000, 0);
  uint32_t x = seed;
  for (size_t i = 0x100; i < 0xf000; ++i) { x = x * 1103515245u + 12345u; d[i] = uint8_t(x >> 16); }
  const uint32_t end = 0x100;
  put32(&d[0], end);   put32(&d[4], 0x000000); put32(&d[8], 0x1fff);    // ILM: bin = the header's end
  put32(&d[12], 0x4000); put32(&d[16], 0x040000); put32(&d[20], 0x0fff); // DLM
  d[0x20] = cascade2nd ? 0x02 : 0x00;
  d[0x28] = 0;
  // Info record 0: the header itself (bin 0 < end) to SRAM 0x80000, 0x3f bytes; the rest empty.
  put32(&d[0x30], 0x080000); put32(&d[0x34], 0x3f); put32(&d[0x38], 0); put32(&d[0x3c], 0x12345678);
  if (cascade2nd) { put32(&d[end - 0x10], 0x090000); put32(&d[end - 0x0c], 0x1f); put32(&d[end - 0x08], 0x40); put32(&d[end - 0x04], 0); }
  d[0xf000] = 0x17; d[0xf001] = 0xe8;   // version and its complement
  d[0xfffd] = 'N'; d[0xfffe] = 'V'; d[0xffff] = 'T';
  return d;
}

struct Rig {
  ChipModel m;
  ByteBus bb{&m};
  std::vector<uint8_t> tx = std::vector<uint8_t>(63 * 1024 + 8), rx = std::vector<uint8_t>(kMaxFrame + 8);
  Chip c;
  explicit Rig(bool bits)
  {
    c.tx = tx.data();
    c.rx = rx.data();
    c.txBytes = uint32_t(tx.size());
    c.rxBytes = uint32_t(rx.size());
    if (bits) c.bus = Bus{nullptr, bitXfer, ByteBus::sleep, 512, 64};
    else c.bus = Bus{&bb, ByteBus::xfer, ByteBus::sleep, 63 * 1024, kMaxFrame - 2};
    if (bits) c.bus.ctx = &bb;
  }
};

void testParse()
{
  for (bool casc : {false, true}) {
    const std::vector<uint8_t> d = syntheticImage(casc, 7);
    Image im;
    CHECK(parseImage(d.data(), uint32_t(d.size()), im) == kOk);
    CHECK(im.bytes == 0x10000 && im.version == 0x17 && im.cascade2nd == casc);
    CHECK(im.part[0].bin == 0x100 && im.part[0].size == 0x1fff && im.part[1].sram == 0x040000 && im.part[1].size == 0x0fff);
    CHECK(im.part[0].crc == crc32Words(d.data() + 0x100, 0x1fff, uint32_t(d.size()) - 0x100));
    CHECK(im.count == 2 + (casc ? (0x100 / 2 - 0x30 + 15) / 16 + 1 : (0x100 - 0x30) / 16));
    CHECK(im.part[2].sram == 0x080000 && im.part[2].size == 0x3f);
    if (casc) CHECK(im.part[3].sram == 0x090000 && im.part[3].bin == 0x40);   // after the header: the cascaded second header
    std::vector<uint8_t> bad = d;
    bad[0xf001] ^= 1;
    CHECK(parseImage(bad.data(), uint32_t(bad.size()), im) == kErrImage);
    bad = d;
    bad[0xffff] = 'X';
    CHECK(parseImage(bad.data(), uint32_t(bad.size()), im) == kErrImage);
    CHECK(parseImage(d.data(), 0x1000, im) == kErrParam);
  }
}

// identify, download, start, frames: the whole bring-up on the model.
bool bringUp(Rig &r, const std::vector<uint8_t> &fw, const char *what)
{
  Image im;
  CHECK(parseImage(fw.data(), uint32_t(fw.size()), im) == kOk);
  CHECK(identify(r.c) == kOk);
  CHECK(r.c.table == int(r.m.answerTable) && r.c.e && r.c.cascade && r.c.eventBuf == kEventCascadeE && r.c.pollInfo == kPollInfoCascadeE);
  CHECK(r.m.engResets == 1);
  const int rc = download(r.c, im, fw.data(), uint32_t(fw.size()));
  if (rc) std::printf("  %s: download rc %d at attempt %u stage %u (model boot checks %u, failed %u)\n", what, rc, r.c.attempt, r.c.stage,
                      r.m.bootChecks, r.m.bootFails);
  CHECK(rc == kOk && r.m.st == ChipModel::St::running && r.c.resetState >= 0xa0);
  CHECK(r.c.attempt == 1 && r.m.bootFails == 0);
  CHECK(start(r.c, true, 2) == kOk && r.c.baselineRc == kOk && r.c.modeRc == kOk);
  CHECK(r.c.payload == 5192 && r.c.payloadReported == 5192 && r.c.fwInfoValid && r.c.fwVersion == 0x21 && r.c.calibrated);
  CHECK(r.m.hostCmds.size() == 2 && r.m.hostCmds[0].first == kCmdIdleBaseline && r.m.hostCmds[1].first == kCmdOpMode &&
        r.m.hostCmds[1].second == 2);
  return rc == kOk;
}

void testBringUpBytes()
{
  for (unsigned table = 0; table < 3; ++table)
    for (bool casc : {false, true}) {
      Rig r(false);
      r.m.answerTable = table;
      bringUp(r, syntheticImage(casc, 11 + table), "synthetic");
      CHECK(r.bb.maxLen <= 63 * 1024 + 1);
      std::vector<uint8_t> f(kMaxFrame + 8);
      r.m.produce(3, [](uint32_t, uint32_t) { return 0; });
      CHECK(readFrame(r.c, f.data()) == kOk && f[0] == 0);
      CHECK(frameKind(f.data()) == FrameKind::data && packetChecksum(f.data() + kTransportBytes, r.c.payload) == 1);
      const PacketFields pf = packetFields(f.data() + kTransportBytes);
      CHECK(pf.marker == 0xa5 && pf.rows == 40 && pf.cols == 60 && pf.type == 3 && pf.counter == 0);
      r.m.produce(3, [](uint32_t, uint32_t) { return 0; }, true);
      CHECK(readFrame(r.c, f.data()) == kOk && packetChecksum(f.data() + kTransportBytes, r.c.payload) == 0);
      r.m.produce(3, [](uint32_t, uint32_t) { return 0; }, false, 0x77);
      CHECK(readFrame(r.c, f.data()) == kOk && frameKind(f.data()) == FrameKind::handshake);
      r.m.produce(3, [](uint32_t, uint32_t) { return 0; }, false, 0xfd);
      CHECK(readFrame(r.c, f.data()) == kOk && frameKind(f.data()) == FrameKind::watchdog);
    }
  // Commands never taken: start still completes, the results say so (5 tries each).
  {
    Rig r(false);
    r.m.takeCommands = false;
    const std::vector<uint8_t> fw = syntheticImage(false, 9);
    Image im;
    parseImage(fw.data(), uint32_t(fw.size()), im);
    CHECK(identify(r.c) == kOk && download(r.c, im, fw.data(), uint32_t(fw.size())) == kOk);
    CHECK(start(r.c, true, 2) == kOk && r.c.baselineRc == kErrTimeout && r.c.modeRc == kErrTimeout && r.m.hostCmds.size() == 10);
    CHECK(start(r.c, false, 0x10000) == kOk && r.c.baselineRc == 1 && r.c.modeRc == 1);
  }
  // No controller: the trim never matches.
  {
    Rig r(false);
    r.m.id[3] = 0x00;
    CHECK(identify(r.c) == kErrChip && r.c.tries == 15);
  }
  // A corrupt SRAM write (the model drops the CRC on): three attempts, then the reset state is never reached.
  {
    Rig r(false);
    const std::vector<uint8_t> fw = syntheticImage(false, 5);
    Image im;
    parseImage(fw.data(), uint32_t(fw.size()), im);
    CHECK(identify(r.c) == kOk);
    im.part[0].crc ^= 1;
    CHECK(download(r.c, im, fw.data(), uint32_t(fw.size())) == kErrTimeout && r.m.bootChecks == 3 && r.m.bootFails == 3 && r.c.stage == 8);
  }
}

void testBringUpBits()
{
  Rig r(true);
  BitSlave s{&r.m};
  gSlave = &s;
  bringUp(r, syntheticImage(true, 3), "synthetic by hand");
  std::vector<uint8_t> f(kMaxFrame + 8);
  r.m.produce(3, [](uint32_t rr, uint32_t cc) { return rr == 5 && cc == 7 ? 500 : 0; });
  CHECK(readFrame(r.c, f.data()) == kOk && packetChecksum(f.data() + kTransportBytes, r.c.payload) == 1);
  CHECK(le16(f.data() + kTransportBytes + 64 + (5 * 60 + 7) * 2) >= 1496);
  CHECK(s.edges > 100000);
}

void testRealImages(int argc, char **argv)
{
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.empty()) { std::printf("FAIL: cannot read image %d\n", i); ++failures; continue; }
    Image im;
    const int rc = parseImage(d.data(), uint32_t(d.size()), im);
    CHECK(rc == kOk);
    if (rc) continue;
    uint32_t total = 0;
    for (uint32_t k = 0; k < im.count; ++k) total += im.part[k].size ? im.part[k].size + 1 : 0;
    Rig r(false);
    const bool ok = bringUp(r, d, "image");
    std::printf("  image %d: %zu bytes, %u partitions, %u bytes to SRAM, cascaded second header %s: model download %s\n", i, d.size(),
                im.count, total, im.cascade2nd ? "yes" : "no", ok ? "passes the CRC check" : "FAILED");
  }
}

// ------------------------------------------------------------------------------------------------------------- the matrix
std::vector<uint8_t> packet(uint8_t type, uint16_t no, int32_t (*extra)(uint32_t, uint32_t), bool badSum = false)
{
  ChipModel m;
  m.counter = no;
  m.produce(type, [extra](uint32_t r, uint32_t c) { return extra ? extra(r, c) : 0; }, badSum);
  std::vector<uint8_t> p(m.payload);
  for (uint32_t i = 0; i < m.payload; ++i) p[i] = m.get(m.eventBuf() + 256 + i);
  return p;
}
int32_t blob(uint32_t r, uint32_t c)
{
  if (r == 10 && c == 45) return 600;
  return r >= 9 && r <= 11 && c >= 44 && c <= 46 ? 300 : 0;
}
int32_t blobAndNoisyRow(uint32_t r, uint32_t c) { return blob(r, c) + (r == 30 ? 150 : 0); }
int32_t drift(uint32_t, uint32_t) { return 40; }

void testMatrix()
{
  auto m = std::make_unique<Matrix>();
  MatrixConfig flipped;
  flipped.flipY = true;   // the CSOT orientation (Project-SunUEFI's test case)
  matrixReset(*m, flipped);
  uint16_t no = 0;
  auto feed = [&](uint8_t type, int32_t (*extra)(uint32_t, uint32_t), bool bad = false) {
    const std::vector<uint8_t> p = packet(type, no++, extra, bad);
    return matrixFeed(*m, p.data(), uint32_t(p.size()));
  };
  CHECK(feed(1, nullptr, true) == Feed::badChecksum && m->badChecksum == 1);
  for (unsigned n = 0; n < 8; ++n) CHECK(feed(6, nullptr) == Feed::learning);   // pen frames never become the type
  for (unsigned n = 0; n < kProbeFrames; ++n) CHECK(feed(1, nullptr) == Feed::learning);
  CHECK(m->refType == 1);
  for (unsigned n = 0; n < kRefFrames; ++n) CHECK(feed(1, nullptr) == Feed::learning);
  CHECK(m->haveReference && m->rows == 40 && m->cols == 60);
  CHECK(feed(1, nullptr) == Feed::decoded && !m->touching);
  CHECK(feed(1, blobAndNoisyRow) == Feed::decoded && m->touching);
  const int32_t x = int32_t((45 * 1024 + 512) * 3200ll / (60 * 1024)), y = 2136 - int32_t((10 * 1024 + 512) * 2136ll / (40 * 1024));
  CHECK(m->x >= x - 2 && m->x <= x + 2 && m->y >= y - 2 && m->y <= y + 2);
  CHECK(y > 2136 / 2 && !m->calibrated);   // reversed: row 10 of 40 lands in the lower half
  CHECK(feed(2, blob) == Feed::otherType && m->touching);
  CHECK(feed(1, nullptr) == Feed::decoded && !m->touching);
  for (unsigned n = 0; n < 20; ++n) CHECK(feed(1, drift) == Feed::decoded && !m->touching);   // a uniform shift is no touch
  // No flip (the default, BOE): Y along the rows as they come.
  MatrixConfig cfg;
  matrixReset(*m, cfg);
  for (unsigned n = 0; n < kProbeFrames + kRefFrames; ++n) feed(1, nullptr);
  CHECK(feed(1, blob) == Feed::decoded && m->touching && m->y >= 2136 - y - 2 && m->y <= 2136 - y + 2);
  // The tip: down on the second frame with a contact, up after three without.
  Tip t;
  CHECK(tipUpdate(t, true, 100, 100) == TipEvent::none);
  CHECK(tipUpdate(t, true, 101, 100) == TipEvent::down && t.down && t.x == 101);
  CHECK(tipUpdate(t, true, 101, 100) == TipEvent::none);
  CHECK(tipUpdate(t, true, 150, 120) == TipEvent::move && t.x == 150 && t.y == 120);
  CHECK(tipUpdate(t, false, 0, 0) == TipEvent::none && tipUpdate(t, false, 0, 0) == TipEvent::none);
  CHECK(tipUpdate(t, true, 151, 120) == TipEvent::move);   // a dropout of two frames keeps the drag
  CHECK(tipUpdate(t, false, 0, 0) == TipEvent::none && tipUpdate(t, false, 0, 0) == TipEvent::none);
  CHECK(tipUpdate(t, false, 0, 0) == TipEvent::up && !t.down && t.x == 151);
  CHECK(tipUpdate(t, true, 10, 10) == TipEvent::none && tipUpdate(t, false, 0, 0) == TipEvent::none);   // one noisy frame: no click
  CHECK(tipUpdate(t, true, 10, 10) == TipEvent::none && tipUpdate(t, true, 10, 10) == TipEvent::down);
  CHECK(tipCancel(t) == TipEvent::up && tipCancel(t) == TipEvent::none && t.downs == 2 && t.ups == 2);
}

// ------------------------------------------------------------------------------------------------------------- calibration
// The synthetic calibration (tests/synthetic-calibration.json uses the same formulas): non-uniform steps of 51..55 px.
Calibration synthetic()
{
  Calibration c{};
  c.present = true;
  for (int i = 0; i < 40; ++i) c.rowMap[i] = 26 + 53 * i + (i % 3 == 0 ? 1 : 0);
  for (int i = 0; i < 60; ++i) c.columnMap[i] = 26 + 53 * i + (i * 2) % 3;
  c.rowMaximum = 21360;
  c.columnMaximum = 32000;
  c.edgeBase = 2700;
  c.edgeSize = 600;
  c.edgeGain = 4350;
  c.projectVendorId = kVendorBoe;
  c.invertY = -1;
  return c;
}
// piano-thp's domainContact over a blob's 3 x 3 nodes (weights delta - 25), computed here independently, pixels x 10.
int32_t weighted(const int32_t *map, const int32_t weights[3], int first)
{
  int64_t sum = 0, w = 0;
  for (int k = 0; k < 3; ++k) {
    sum += int64_t(weights[k]) * map[first + k] * 10;
    w += weights[k];
  }
  return int32_t((2 * sum + w) / (2 * w));
}
int32_t blobAt(uint32_t r, uint32_t c, uint32_t r0, uint32_t c0)
{
  if (r == r0 && c == c0) return 600;
  return int32_t(r) >= int32_t(r0) - 1 && r <= r0 + 1 && int32_t(c) >= int32_t(c0) - 1 && c <= c0 + 1 ? 300 : 0;
}

void testCalibration()
{
  const Calibration c = synthetic();
  CHECK(calibrationValid(c));
  {
    Calibration bad = c;
    bad.rowMap[7] = bad.rowMap[6];
    CHECK(!calibrationValid(bad));
    bad = c;
    bad.columnMap[59] = 3200;
    CHECK(!calibrationValid(bad));
    bad = c;
    bad.edgeSize = 10680;
    CHECK(!calibrationValid(bad));
    bad = c;
    bad.present = false;
    CHECK(!calibrationValid(bad));
  }
  // The edge mapping: identity inside, stretched to the ends, monotonic, the low edge of Y one unit lower.
  CHECK(mapEdgeAxis(5000, 32000, false, c) == 5000 && mapEdgeAxis(600, 32000, false, c) == 600 && mapEdgeAxis(31400, 32000, false, c) == 31400);
  CHECK(mapEdgeAxis(0, 32000, false, c) == 1 && mapEdgeAxis(0, 21360, true, c) == 0 && mapEdgeAxis(31999, 32000, false, c) == 31999);
  CHECK(mapEdgeAxis(400, 32000, false, c) == 600 - 4350 * 200 / 2700 && mapEdgeAxis(400, 32000, false, c) < 400);
  CHECK(mapEdgeAxis(31700, 32000, false, c) == 31400 + 4350 * 300 / 2700 && mapEdgeAxis(31700, 32000, false, c) > 31700);
  int32_t last = -1;
  bool monotonic = true;
  for (int32_t v = 0; v < 21360; v += 7) {
    const int32_t m = mapEdgeAxis(v, 21360, true, c);
    monotonic = monotonic && m >= last;
    last = m;
  }
  CHECK(monotonic);
#ifdef PIANO_TOUCH_SYNTHETIC_HEADER
  // The generator's round trip: the JSON file through the header equals the formulas.
  CHECK(kSyntheticCalibrationPresent && calibrationValid(kSyntheticCalibration) && kSyntheticCalibration.invertY == -1 &&
        kSyntheticCalibration.projectVendorId == kVendorBoe && kSyntheticCalibration.edgeGain == c.edgeGain);
  CHECK(std::memcmp(kSyntheticCalibration.rowMap, c.rowMap, sizeof c.rowMap) == 0 &&
        std::memcmp(kSyntheticCalibration.columnMap, c.columnMap, sizeof c.columnMap) == 0);
#endif
  // The centroid exactly, on an asymmetric blob without noise: the weights are delta - 25 (corners of 30 count 5, not 30).
  {
    std::vector<int32_t> d(2400, 0);
    auto at = [&](int r, int cc) -> int32_t & { return d[size_t(r * 60 + cc)]; };
    at(10, 45) = 600; at(10, 44) = 300; at(10, 46) = 40; at(9, 45) = at(11, 45) = 200;
    at(9, 44) = at(11, 44) = at(9, 46) = at(11, 46) = 30;
    int32_t x10 = 0, y10 = 0;
    calibratedCentroid(c, d.data(), 10, 45, &x10, &y10);
    const int64_t wx[3] = {275 + 5 + 5, 575 + 175 + 175, 15 + 5 + 5}, wy[3] = {175 + 5 + 5, 275 + 575 + 15, 175 + 5 + 5};
    int64_t sx = 0, sy = 0, w = 0;
    for (int k = 0; k < 3; ++k) {
      sx += wx[k] * c.columnMap[44 + k] * 10;
      sy += wy[k] * c.rowMap[9 + k] * 10;
      w += wx[k];
    }
    CHECK(x10 == int32_t((2 * sx + w) / (2 * w)) && y10 == int32_t((2 * sy + w) / (2 * w)));
    at(10, 45) = 20; at(10, 44) = at(10, 46) = at(9, 45) = at(11, 45) = 0;
    at(9, 44) = at(11, 44) = at(9, 46) = at(11, 46) = 0;
    calibratedCentroid(c, d.data(), 10, 45, &x10, &y10);
    CHECK(x10 == c.columnMap[45] * 10 && y10 == c.rowMap[10] * 10);   // nothing above the offset: the peak's own electrode
  }
  // Contacts through the matrix with the calibration: interior (no edge mapping), at an edge, reversed Y, and the fallback.
  auto m = std::make_unique<Matrix>();
  auto learn = [&](MatrixConfig cfg) {
    matrixReset(*m, cfg);
    for (uint16_t no = 0; no < kProbeFrames + kRefFrames; ++no) {
      const std::vector<uint8_t> p = packet(1, no, nullptr);
      matrixFeed(*m, p.data(), uint32_t(p.size()));
    }
  };
  auto touch = [&](uint32_t r0, uint32_t c0) {
    ChipModel model;
    model.counter = 200;
    model.produce(1, [r0, c0](uint32_t r, uint32_t cc) { return blobAt(r, cc, r0, c0); });
    std::vector<uint8_t> p(model.payload);
    for (uint32_t i = 0; i < model.payload; ++i) p[i] = model.get(model.eventBuf() + 256 + i);
    return matrixFeed(*m, p.data(), uint32_t(p.size())) == Feed::decoded && m->touching;
  };
  const int32_t sides[3] = {3 * 275, 2 * 275 + 575, 3 * 275};   // column (or row) weights of the 3 x 3 blob
  MatrixConfig cfg;
  cfg.cal = &c;
  learn(cfg);
  CHECK(m->calibrated);
  CHECK(touch(10, 45));
  int32_t ex = weighted(c.columnMap, sides, 44) / 10, ey = weighted(c.rowMap, sides, 9) / 10;
  CHECK(m->x >= ex - 1 && m->x <= ex + 1 && m->y >= ey - 1 && m->y <= ey + 1);
  const int32_t linearX = int32_t((45 * 1024 + 512) * 3200ll / (60 * 1024)), linearY = int32_t((10 * 1024 + 512) * 2136ll / (40 * 1024));
  CHECK(ex != linearX && ey != linearY);   // the non-uniform maps move the contact off the linear node centre
  // Row 20, column 0: X inside the edge band (stretched toward 0), Y interior. The blob's columns 0 and 1 weigh 1125 and 825.
  CHECK(touch(20, 0));
  const int32_t rawX = int32_t((2 * (1125ll * c.columnMap[0] * 10 + 825ll * c.columnMap[1] * 10) + 1950) / (2 * 1950));
  CHECK(rawX < 600 && m->x >= mapEdgeAxis(rawX, 32000, false, c) / 10 - 1 && m->x <= mapEdgeAxis(rawX, 32000, false, c) / 10 + 1);
  CHECK(m->x < rawX / 10);
  // Reversed Y (CSOT): rowMaximum - 1 - y, in tenths of a pixel.
  cfg.flipY = true;
  learn(cfg);
  CHECK(touch(10, 45));
  const int32_t fy = (21360 - 1 - weighted(c.rowMap, sides, 9)) / 10;
  CHECK(m->y >= fy - 1 && m->y <= fy + 1 && m->x >= ex - 1 && m->x <= ex + 1);
  // An invalid calibration: the linear map, as without one.
  Calibration broken = c;
  broken.rowMap[3] = 0;
  cfg.cal = &broken;
  cfg.flipY = false;
  learn(cfg);
  CHECK(!m->calibrated && touch(10, 45) && m->x >= linearX - 2 && m->x <= linearX + 2 && m->y >= linearY - 2 && m->y <= linearY + 2);
#ifdef PIANO_TOUCH_UNIT_HEADER
  if (kUnitCalibrationPresent) {
    // The unit's file: valid, a piano panel, 40..70 px electrode pitch, a centred contact inside the panel. Nothing printed.
    const Calibration &u = kUnitCalibration;
    bool pitch = true;
    for (int i = 1; i < 40; ++i) pitch = pitch && u.rowMap[i] - u.rowMap[i - 1] >= 40 && u.rowMap[i] - u.rowMap[i - 1] <= 70;
    for (int i = 1; i < 60; ++i) pitch = pitch && u.columnMap[i] - u.columnMap[i - 1] >= 40 && u.columnMap[i] - u.columnMap[i - 1] <= 70;
    CHECK(calibrationValid(u) && (u.projectVendorId == kVendorBoe || u.projectVendorId == kVendorCsot) && pitch);
    cfg.cal = &u;
    learn(cfg);
    CHECK(m->calibrated && touch(20, 30) && m->x > 1200 && m->x < 2000 && m->y > 700 && m->y < 1400);
    std::printf("  unit calibration: valid, panel %s, a centred contact lands mid-panel\n", u.projectVendorId == kVendorBoe ? "BOE" : "CSOT");
  }
#endif
}

void testOrientation()
{
  const Calibration boe = synthetic();
  Calibration csot = boe, invert = boe, unknown = boe, broken = boe;
  csot.projectVendorId = kVendorCsot;
  invert.invertY = 1;
  unknown.projectVendorId = -1;
  broken.edgeBase = 0;
  constexpr uint32_t kAuto = 4;
  Orientation o = chooseOrientation(kPanelBoe, nullptr, kAuto);
  CHECK(!o.flipX && !o.flipY && o.panel == kPanelBoe && !o.mismatch);
  o = chooseOrientation(kPanelCsot, nullptr, kAuto);
  CHECK(!o.flipX && o.flipY && o.panel == kPanelCsot);
  o = chooseOrientation(kPanelBoe, &boe, kAuto);
  CHECK(!o.flipY && !o.mismatch && o.calibrationPanel == kPanelBoe);
  o = chooseOrientation(kPanelBoe, &csot, kAuto);   // they disagree: the calibration decides the orientation
  CHECK(o.mismatch && o.panel == kPanelCsot && o.flipY);
  o = chooseOrientation(kPanelCsot, &boe, kAuto);
  CHECK(o.mismatch && o.panel == kPanelBoe && !o.flipY);
  o = chooseOrientation(kPanelUnknown, &boe, kAuto);
  CHECK(!o.mismatch && o.panel == kPanelBoe && !o.flipY);
  o = chooseOrientation(kPanelUnknown, nullptr, kAuto);   // nothing known: CSOT, as piano-thp without a panel report
  CHECK(o.panel == kPanelCsot && o.flipY);
  o = chooseOrientation(kPanelBoe, &invert, kAuto);
  CHECK(o.flipY && !o.flipX);
  o = chooseOrientation(kPanelCsot, &unknown, kAuto);
  CHECK(!o.mismatch && o.panel == kPanelCsot && o.flipY);
  o = chooseOrientation(kPanelBoe, &broken, kAuto);   // an invalid calibration counts as none
  CHECK(o.calibrationPanel == kPanelUnknown && !o.flipY);
  for (uint32_t f = 0; f < 4; ++f) {
    o = chooseOrientation(kPanelBoe, &csot, f);
    CHECK(o.flipX == bool(f & 1) && o.flipY == bool(f & 2));
  }
}

// ------------------------------------------------------------------------------------------------------------- HID, clocks
void testHid()
{
  // Walk the items: global report size/count, the input items' bits.
  unsigned size = 0, count = 0, inputBits = 0, collections = 0, ends = 0;
  int32_t logicalMax = 0;
  for (size_t i = 0; i < sizeof kTabletDescriptor;) {
    const uint8_t prefix = kTabletDescriptor[i];
    const unsigned n = prefix & 3 ? 1u << ((prefix & 3) - 1) : 0;
    uint32_t v = 0;
    for (unsigned k = 0; k < n; ++k) v |= uint32_t(kTabletDescriptor[i + 1 + k]) << (8 * k);
    switch (prefix & 0xfc) {
    case 0x74: size = v; break;
    case 0x94: count = v; break;
    case 0x80: inputBits += size * count; break;
    case 0xa0: ++collections; break;
    case 0xc0: ++ends; break;
    case 0x24: logicalMax = int32_t(v); break;
    default: break;
    }
    i += 1 + n;
  }
  CHECK(inputBits == kReportBytes * 8 && collections == 2 && ends == 2 && logicalMax == kHidMax);
  CHECK(kTabletDescriptor[0] == 0x05 && kTabletDescriptor[1] == 0x01 && kTabletDescriptor[3] == 0x02);
  uint8_t r[kReportBytes];
  tabletReport(true, 0, 0, kPanelW, kPanelH, r);
  CHECK(r[0] == 1 && r[1] == 0 && r[2] == 0 && r[3] == 0 && r[4] == 0);
  tabletReport(false, kPanelW - 1, kPanelH - 1, kPanelW, kPanelH, r);
  CHECK(r[0] == 0 && le16(r + 1) == kHidMax && le16(r + 3) == kHidMax);
  tabletReport(true, 1600, 5000, kPanelW, kPanelH, r);
  CHECK(le16(r + 1) == uint16_t(1600ll * kHidMax / (kPanelW - 1)) && le16(r + 3) == kHidMax);
}

// piano-touch-rotate=1 (TouchRotation.h, sensors.md 5a.3): at 0 the bytes are tabletReport's for every contact, in range or not; at
// 90/180/270 the panel's corners land on the rotated desktop's (CGDisplayRotation: 90 = the panel's left edge up); a request takes
// effect at the next touch-down only.
void testRotation()
{
  uint8_t a[kReportBytes], b[kReportBytes];
  bool same = true;
  for (int32_t y = -40; y < kPanelH + 40; y += 7)
    for (int32_t x = -40; x < kPanelW + 40; x += 13)
      for (int tip = 0; tip < 2; ++tip) {
        tabletReport(tip, x, y, kPanelW, kPanelH, a);
        rotatedTabletReport(tip, x, y, kPanelW, kPanelH, 0, b);
        same = same && !std::memcmp(a, b, kReportBytes);
      }
  CHECK(same);
  const struct { uint32_t deg; int32_t x, y; uint16_t hx, hy; } corner[] = {
    {90, 0, kPanelH - 1, 0, 0},           // the panel's bottom-left corner is the desktop's top-left
    {90, 0, 0, kHidMax, 0},               // top-left -> top-right
    {90, kPanelW - 1, 0, kHidMax, kHidMax},
    {180, 0, 0, kHidMax, kHidMax},        {180, kPanelW - 1, kPanelH - 1, 0, 0},
    {270, kPanelW - 1, 0, 0, 0},          // the panel's top-right corner is the desktop's top-left
    {270, 0, 0, 0, kHidMax},
  };
  for (const auto &c : corner) {
    rotatedTabletReport(true, c.x, c.y, kPanelW, kPanelH, c.deg, b);
    CHECK(b[0] == 1 && le16(b + 1) == c.hx && le16(b + 3) == c.hy);
  }
  rotatedTabletReport(false, kPanelW / 2, kPanelH / 2, kPanelW, kPanelH, 90, b);   // the centre stays the centre (within 1.5 px)
  CHECK(le16(b + 1) + 16 >= kHidMax / 2 && le16(b + 1) <= kHidMax / 2 + 16 && le16(b + 3) + 16 >= kHidMax / 2 && le16(b + 3) <= kHidMax / 2 + 16);
  rotatedTabletReport(true, 100, 100, kPanelW, kPanelH, 45, b);                    // not a rotation: as 0
  tabletReport(true, 100, 100, kPanelW, kPanelH, a);
  CHECK(!std::memcmp(a, b, kReportBytes));
  PatRotation r;
  pat_init(&r);
  CHECK(pat_request(&r, 90) && pat_latch(&r, 0) == 0 && pat_latch(&r, 1) == 90 && !pat_request(&r, 91) && pat_latch(&r, 1) == 90);
}

void testClock()
{
  CHECK(rcgRate(0, 0, 0) == 19200000);                                  // XO, divider bypassed: the reset state
  CHECK(rcgRate(1u << 8 | 11, 0, 0) == 100000000);                     // GPLL0 600 MHz, 2 x 600 / 12
  CHECK(rcgRate(6u << 8 | 1, 0, 0) == 300000000);                      // GPLL0 even, divider 1
  CHECK(rcgRate(3u << 8, 0, 0) == 0);                                  // an unknown source
  CHECK(rcgRate(2u << 12, 1, uint32_t(~(4 - 1))) == 19200000 / 4);     // M/N 1/4 (N register ~(N - M))
  std::map<uint32_t, uint32_t> g = {{kQspiRefRcg + 4, 1u << 8 | 11}, {kSe2Divider, 1}};
  const Se2Clock c = se2Clock([&](uint32_t off) { return g.count(off) ? g[off] : 0u; });
  CHECK(c.sourceHz == 100000000 && c.seHz == 50000000);
  g[kQspiRefRcg + 0x14] = 1;
  g[kQspiRefRcg + 0x1c] = 0;   // DFS: performance level 0 is XO
  CHECK(se2Clock([&](uint32_t off) { return g.count(off) ? g[off] : 0u; }).seHz == 9600000);
  CHECK(geniDivider(19200000, 9600000) == 2 && geniDivider(19200000, 19200000) == 1 && geniDivider(100000000, 9600000) == 11);
  CHECK(geniDivider(0, 9600000) == 0 && geniDivider(19200000000u / 10, 1) == 0);
  CHECK(branchOn(0x00000001) && branchOn(0x80000002) && !branchOn(0x80000001) && branchOn(0));
  CHECK(kVoteBits == 0x013c0000u && kGpio[kMiso] == 40 && kGpio[kCs] == 43 && kGpio[kIrq] == 162 && kGpio[kLcdId] == 100);
}

// TouchPace in a model of PianoTouch::serve's loop (times in us): frames every 8333 us, a read takes 4500 us and sees the finger
// if it is down at the read; the panel is busy while the last frame had the finger. irq: the edge wait is one wake-up (the
// interrupt), else 1 ms polls. Returns the wake-ups and reads in [from, to), and the first read with the finger after fingerOn.
struct PaceRun {
  unsigned wakeups = 0, reads = 0;
  uint64_t firstSeenUs = 0;
};
static PaceRun simulatePace(uint32_t idleMs, bool irq, uint64_t fingerOn, uint64_t fingerOff, uint64_t from, uint64_t to)
{
  constexpr uint64_t kFrameUs = 8333, kReadUs = 4500;
  TouchPace pace = touchPace(idleMs, 0);
  PaceRun r;
  uint64_t t = 0, lastEdgeTaken = 0, lastReadUs = 0;
  bool touching = false;
  auto lastEdgeAt = [&](uint64_t at) { return at / kFrameUs * kFrameUs; };
  auto count = [&](uint64_t at) { if (at >= from && at < to) ++r.wakeups; };
  while (t < to) {
    const uint64_t edge = lastEdgeAt(t);
    if (edge > lastEdgeTaken && edge > 0) {   // latched: read (1 ms polls see an edge at most 1 ms late)
      lastEdgeTaken = edge;
      if (t >= from && t < to) ++r.reads;
      touching = t >= fingerOn && t < fingerOff;
      if (touching && !r.firstSeenUs) r.firstSeenUs = t;
      lastReadUs = t;
      t += kReadUs;
    }
    const bool idle = pace.idle(t / 1000, touching);
    if (idle) {
      const uint32_t sleep = pace.sleepMs(t / 1000, lastReadUs / 1000);
      if (sleep) {
        t += uint64_t(sleep) * 1000;
        count(t);
        lastEdgeTaken = lastEdgeAt(t);   // the latch dropped
        continue;
      }
      if (irq) {   // armed: the interrupt at the next edge
        t = lastEdgeAt(t) + kFrameUs + 50;
        count(t);
        continue;
      }
    }
    t += 1000;
    count(t);
  }
  return r;
}

static void testPace()
{
  CHECK(TouchPace::clampIdle(0) == 0 && TouchPace::clampIdle(1) == kIdleReadMinMs && TouchPace::clampIdle(9999) == kIdleReadMaxMs);
  {   // the old loop (idle 0): 1000 wake-ups and ~120 reads a second at rest
    const PaceRun r = simulatePace(0, false, ~0ull, ~0ull, 10000000, 20000000);
    CHECK(r.wakeups >= 3000 && r.wakeups <= 10000 && r.reads >= 1190 && r.reads <= 1210);   // 1 ms polls between the reads
  }
  {   // idle 50 ms, polled: about 20 reads and well under 150 wake-ups a second
    const PaceRun r = simulatePace(kIdleReadMs, false, ~0ull, ~0ull, 10000000, 20000000);
    std::printf("pace: idle %u ms, polled: %u wake-ups, %u reads in 10 s\n", kIdleReadMs, r.wakeups, r.reads);
    CHECK(r.reads >= 170 && r.reads <= 210 && r.wakeups <= 1500);
  }
  {   // idle 50 ms, interrupt: two wake-ups a read
    const PaceRun r = simulatePace(kIdleReadMs, true, ~0ull, ~0ull, 10000000, 20000000);
    std::printf("pace: idle %u ms, interrupt: %u wake-ups, %u reads in 10 s\n", kIdleReadMs, r.wakeups, r.reads);
    CHECK(r.reads >= 170 && r.reads <= 210 && r.wakeups <= 2 * r.reads + 10);
  }
  // A finger at any phase is read at most an idle period and a frame (plus a poll) late, then every frame while it stays and for
  // kHoldMs after it lifts.
  for (bool irq : {false, true})
    for (uint64_t phase = 0; phase < 60000; phase += 1777) {
      const uint64_t on = 20000000 + phase, off = on + 500000;
      const PaceRun seen = simulatePace(kIdleReadMs, irq, on, off, 0, on + 200000);
      CHECK(seen.firstSeenUs && seen.firstSeenUs - on <= (kIdleReadMs + 9 + 1) * 1000ull);
      const PaceRun busy = simulatePace(kIdleReadMs, irq, on, off, on + 100000, off);
      CHECK(busy.reads >= 45);   // every frame of the last 400 ms (48 frames)
      const PaceRun hold = simulatePace(kIdleReadMs, irq, on, off, off + 100000, off + kHoldMs * 1000ull - 100000);
      CHECK(hold.reads >= 150);   // still every frame
      const PaceRun after = simulatePace(kIdleReadMs, irq, on, off, off + kHoldMs * 1000ull + 200000, off + kHoldMs * 1000ull + 1200000);
      CHECK(after.reads <= 24);   // idle again
    }
}

int main(int argc, char **argv)
{
  testParse();
  testBringUpBytes();
  testBringUpBits();
  testRealImages(argc, argv);
  testMatrix();
  testCalibration();
  testOrientation();
  testHid();
  testRotation();
  testClock();
  testPace();
  if (failures) {
    std::printf("PianoTouch host tests: %d FAILED\n", failures);
    return 1;
  }
  std::printf("PASS: PianoTouch host tests (NT36532E model over whole transfers and by hand, %d real image(s), matrix, calibration, "
              "orientation, tip, HID, rotation, clocks)\n", argc - 1);
  return 0;
}
