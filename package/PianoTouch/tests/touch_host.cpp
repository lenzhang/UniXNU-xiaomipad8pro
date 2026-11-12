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
#include "TouchTrack.h"
#include "TouchGesture.h"
#include "TouchHid.h"
#include "Se2Bus.h"
#include "TouchPace.h"
#include "TouchPower.h"
#ifdef PIANO_TOUCH_SYNTHETIC_HEADER
#include PIANO_TOUCH_SYNTHETIC_HEADER   // tests/synthetic-calibration.json through tooling/k27/gen-touch-calibration.py
#endif
#ifdef PIANO_TOUCH_UNIT_HEADER
#include PIANO_TOUCH_UNIT_HEADER        // the unit's private calibration (out/ only): properties checked, nothing printed
#endif
#include <algorithm>
#include <climits>
#include <cmath>
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
  // After a suspend or the display's return (TouchPower.h): the probe reads the firmware info and the reset state only. Kept after
  // the start; a controller that lost its SRAM (the panel's power went) reads as lost; the download brings it back; another image
  // (a different version) is not the one start() saw.
  {
    Rig r(false);
    const std::vector<uint8_t> fw = syntheticImage(true, 13);
    CHECK(bringUp(r, fw, "probe"));
    const unsigned transfers = r.m.transfers, hostCmds = unsigned(r.m.hostCmds.size());
    bool kept = false;
    CHECK(probeState(r.c, &kept) == kOk && kept && r.c.probeVersion == 0x21 && r.c.probeReset >= kResetStateInit);
    CHECK(r.m.hostCmds.size() == hostCmds && r.m.transfers > transfers && r.m.bootChecks == 1);   // reads only: no command, no boot
    r.m.st = ChipModel::St::rom;
    for (uint32_t i = 0; i < 4; ++i) r.m.mem.erase(r.m.eventBuf() + kEvFwInfo + i);
    r.m.mem[r.m.eventBuf() + kEvResetComplete] = 0;
    CHECK(probeState(r.c, &kept) == kOk && !kept);
    Image im;
    parseImage(fw.data(), uint32_t(fw.size()), im);
    CHECK(download(r.c, im, fw.data(), uint32_t(fw.size())) == kOk && start(r.c, true, 2) == kOk);
    CHECK(probeState(r.c, &kept) == kOk && kept);
    r.m.mem[r.m.eventBuf() + kEvFwInfo] = 0x22;
    r.m.mem[r.m.eventBuf() + kEvFwInfo + 1] = 0xdd;
    CHECK(probeState(r.c, &kept) == kOk && !kept);
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
  // This unit's Y from the build (PIANO_TOUCH_INVERT_Y): before the calibration's invert_y and the panel, after piano-touch-flip.
  o = chooseOrientation(kPanelBoe, &boe, kAuto, 1);   // this BOE unit: Y reversed although the panel says as processor Y
  CHECK(o.flipY && !o.flipX && std::strstr(o.why, "PIANO_TOUCH_INVERT_Y=1"));
  o = chooseOrientation(kPanelUnknown, nullptr, kAuto, 1);   // without a calibration too
  CHECK(o.flipY);
  o = chooseOrientation(kPanelCsot, &invert, kAuto, 0);      // before the file's invert_y and the CSOT default
  CHECK(!o.flipY);
  o = chooseOrientation(kPanelBoe, &boe, 0, 1);              // an explicit piano-touch-flip=0 still wins
  CHECK(!o.flipY && !o.flipX);
  o = chooseOrientation(kPanelBoe, &boe, 2, 0);              // and piano-touch-flip=2
  CHECK(o.flipY);
  o = chooseOrientation(kPanelBoe, &boe, kAuto, -1);         // no override: as before
  CHECK(!o.flipY);
#ifdef PIANO_TOUCH_UNIT_HEADER
  CHECK(kUnitInvertYOverride >= -1 && kUnitInvertYOverride <= 1);
#endif
  for (uint32_t f = 0; f < 4; ++f) {
    o = chooseOrientation(kPanelBoe, &csot, f);
    CHECK(o.flipX == bool(f & 1) && o.flipY == bool(f & 2));
  }
}

// ------------------------------------------------------------------------------------------------------------- HID, clocks
// The descriptor read item by item as IOHIDFamily would: per report ID the input bits, each input field's usage, ranges, unit
// exponent and relative flag; the Wheel's resolution as IOHIDEventService::determineResolution computes it (logical range over
// physical range, a negative exponent's power of ten on the logical side, an integer quotient; none when a physical bound equals the
// logical one), as PianoKeyboard's host test reads its touchpad descriptor.
struct HidField {
  uint32_t usage = 0, usageMax = 0;   // page << 16 | usage (a range for the buttons and the modifiers)
  int32_t lmin = 0, lmax = 0, pmin = 0, pmax = 0, exponent = 0;
  uint32_t unit = 0, bits = 0, count = 0;
  bool relative = false, constant = false;
  uint8_t reportId = 0;
};
struct HidParse {
  std::vector<HidField> fields;
  std::map<uint8_t, unsigned> inputBits;
  unsigned collections = 0, applications = 0;
  int depth = 0;
};
static int32_t signedItem(uint32_t v, unsigned len) { return len == 1 ? int8_t(v) : len == 2 ? int16_t(v) : int32_t(v); }
static HidParse parseHid(const uint8_t *d, unsigned n)
{
  HidParse out;
  int32_t lmin = 0, lmax = 0, pmin = 0, pmax = 0, exponent = 0;
  uint32_t unit = 0, size = 0, count = 0, page = 0, umin = 0, umax = 0;
  uint8_t id = 0;
  std::vector<uint32_t> usages;
  for (unsigned i = 0; i < n;) {
    const uint8_t item = d[i];
    const unsigned len = (item & 3) == 3 ? 4 : item & 3;
    uint32_t v = 0;
    for (unsigned k = 0; k < len; ++k) v |= uint32_t(d[i + 1 + k]) << (8 * k);
    switch (item & 0xfc) {
    case 0x04: page = v; break;
    case 0x08: usages.push_back(len == 4 ? v : page << 16 | v); break;
    case 0x18: umin = page << 16 | v; break;
    case 0x28: umax = page << 16 | v; break;
    case 0x14: lmin = signedItem(v, len); break;
    case 0x24: lmax = signedItem(v, len); break;
    case 0x34: pmin = signedItem(v, len); break;
    case 0x44: pmax = signedItem(v, len); break;
    case 0x54: exponent = int32_t(v & 0x0f); break;
    case 0x64: unit = v; break;
    case 0x74: size = v; break;
    case 0x84: id = uint8_t(v); break;
    case 0x94: count = v; break;
    case 0xa0:
      ++out.depth;
      ++out.collections;
      out.applications += v == 1;
      usages.clear();
      break;
    case 0xc0: --out.depth; break;
    case 0x80: {
      out.inputBits[id] += size * count;
      HidField f;
      f.lmin = lmin; f.lmax = lmax; f.pmin = pmin; f.pmax = pmax; f.exponent = exponent; f.unit = unit; f.bits = size;
      f.count = count; f.relative = v & 4; f.constant = v & 1; f.reportId = id;
      if (!f.constant && usages.empty()) {
        f.usage = umin;
        f.usageMax = umax;
        out.fields.push_back(f);
      }
      for (uint32_t u : usages) {
        f.usage = f.usageMax = u;
        out.fields.push_back(f);
      }
      usages.clear();
      umin = umax = 0;
      break;
    }
    default: break;
    }
    i += 1 + len;
  }
  return out;
}
static const HidField *hidField(const HidParse &h, uint32_t usage)
{
  for (const HidField &f : h.fields)
    if (f.usage == usage) return &f;
  return nullptr;
}
static int32_t hidResolution(const HidField &a)
{
  if (!a.relative || a.pmin == a.lmin || a.pmax == a.lmax) return 0;
  int64_t logical = a.lmax - a.lmin, physical = a.pmax - a.pmin;
  if (a.exponent < 8) for (int i = a.exponent; i > 0; --i) physical *= 10;
  else for (int i = 0x10 - a.exponent; i > 0; --i) logical *= 10;
  return int32_t(logical / physical);
}

void testHid()
{
  for (bool keyboard : {false, true})
    for (uint32_t res : {45u, 10u, 90u, 900u, 3u, 5000u}) {
      const TouchDescriptor d = makeTouchDescriptor(res, keyboard);
      CHECK(d.length <= sizeof d.bytes);
      const HidParse h = parseHid(d.bytes, d.length);
      const uint8_t pid = keyboard ? kPointerReportId : 0;
      CHECK(h.depth == 0 && h.applications == (keyboard ? 2u : 1u) && h.collections == (keyboard ? 3u : 2u));
      CHECK(h.inputBits.size() == (keyboard ? 2u : 1u) && h.inputBits.at(pid) == kPointerBytes * 8);
      const HidField *buttons = hidField(h, 0x090001), *x = hidField(h, 0x010030), *y = hidField(h, 0x010031);
      const HidField *wheel = hidField(h, 0x010038), *pan = hidField(h, 0x0c0238);
      CHECK(buttons && buttons->usageMax == 0x090003 && buttons->count == 3 && !buttons->relative && buttons->reportId == pid);
      CHECK(x && y && !x->relative && !y->relative && x->lmin == 0 && x->lmax == kHidMax && y->lmax == kHidMax && x->bits == 16);
      CHECK(x && x->pmax == x->lmax);   // positions: no resolution
      CHECK(wheel && pan && wheel->relative && pan->relative && wheel->lmin == -127 && wheel->lmax == 127 && pan->bits == 8);
      CHECK(wheel && hidResolution(*wheel) == int32_t(touchScrollResolution(res)) && wheel->unit == 0x13);
      if (keyboard) {
        CHECK(h.inputBits.at(kKeyboardReportId) == 8);
        const HidField *mods = hidField(h, 0x0700e0);
        CHECK(mods && mods->usageMax == 0x0700e7 && mods->count == 8 && mods->reportId == kKeyboardReportId && mods->unit == 0 &&
              mods->exponent == 0);
      }
    }
  // The default the HID device starts from: 45 counts an inch, no keyboard; its first byte the GD page, the mouse usage.
  CHECK(kTouchDescriptorDefault.length == makeTouchDescriptor(kTouchScrollResolution, false).length);
  CHECK(kTouchDescriptorDefault.bytes[0] == 0x05 && kTouchDescriptorDefault.bytes[1] == 0x01 && kTouchDescriptorDefault.bytes[3] == 0x02);
  // The reports: corners scale to 0 and 32767, counts clamp to a signed byte, the report ID first when there are two reports.
  uint8_t r[kMaxReportBytes];
  CHECK(pointerReport(PointerReport{kButtonLeft, 0, 0, 0, 0, 0}, kPanelW, kPanelH, false, r) == kPointerBytes);
  CHECK(r[0] == 1 && le16(r + 1) == 0 && le16(r + 3) == 0 && r[5] == 0 && r[6] == 0);
  CHECK(pointerReport(PointerReport{kButtonRight, kPanelW - 1, kPanelH - 1, 5, -3, 0}, kPanelW, kPanelH, false, r) == kPointerBytes);
  CHECK(r[0] == 2 && le16(r + 1) == kHidMax && le16(r + 3) == kHidMax && int8_t(r[5]) == 5 && int8_t(r[6]) == -3);
  CHECK(pointerReport(PointerReport{0, 1600, 5000, 900, -900, 0}, kPanelW, kPanelH, true, r) == kPointerBytes + 1);
  CHECK(r[0] == kPointerReportId && r[1] == 0 && le16(r + 2) == uint16_t(1600ll * kHidMax / (kPanelW - 1)) && le16(r + 4) == kHidMax);
  CHECK(int8_t(r[6]) == 127 && int8_t(r[7]) == -127);
  CHECK(modifierReport(kModifierCommand, r) == 2 && r[0] == kKeyboardReportId && r[1] == 0x08);
  tabletReport(true, 0, 0, kPanelW, kPanelH, r);
  CHECK(r[0] == 1 && r[1] == 0 && r[2] == 0 && r[3] == 0 && r[4] == 0);
}

// piano-touch-rotate=1 (TouchRotation.h, sensors.md 5a.3): at 0 the bytes are tabletReport's for every contact, in range or not; at
// 90/180/270 the panel's corners land on the rotated desktop's (CGDisplayRotation: 90 = the panel's left edge up); a request takes
// effect at the next touch-down only.
void testRotation()
{
  uint8_t a[kTabletBytes], b[kTabletBytes];
  bool same = true;
  for (int32_t y = -40; y < kPanelH + 40; y += 7)
    for (int32_t x = -40; x < kPanelW + 40; x += 13)
      for (int tip = 0; tip < 2; ++tip) {
        tabletReport(tip, x, y, kPanelW, kPanelH, a);
        rotatedTabletReport(tip, x, y, kPanelW, kPanelH, 0, b);
        same = same && !std::memcmp(a, b, kTabletBytes);
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
  CHECK(!std::memcmp(a, b, kTabletBytes));
  PatRotation r;
  pat_init(&r);
  CHECK(pat_request(&r, 90) && pat_latch(&r, 0) == 0 && pat_latch(&r, 1) == 90 && !pat_request(&r, 91) && pat_latch(&r, 1) == 90);
}

// ------------------------------------------------------------------------------------------------------- contacts and gestures
// Synthetic scenes: contacts as Gaussian blobs at panel positions (pixels; the linear map, node centres at (i + 1/2) / n of the
// panel) on the model's baseline and its +-4 noise, made straight into a THP packet (the ChipModel's layout and checksum).
struct Spot {
  double x, y, amp = 600, sigma = 0.8;   // panel pixels; the peak's delta; the spread in nodes
};
constexpr double kNodeW = double(kPanelW) / 60, kNodeH = double(kPanelH) / 40;
static std::vector<uint8_t> scenePacket(uint16_t counter, const std::vector<Spot> &spots, uint8_t type = 1)
{
  constexpr uint32_t payload = 5192;
  std::vector<uint8_t> p(payload, 0);
  p[0] = 0xa5;
  put16(&p[2], counter);
  p[24] = 2;
  put16(&p[28], counter);
  p[48] = 60;
  p[49] = 40;
  p[56] = type;
  for (uint32_t r = 0; r < 40; ++r)
    for (uint32_t c = 0; c < 60; ++c) {
      double v = 0;
      for (const Spot &s : spots) {
        const double dx = ((c + 0.5) * kNodeW - s.x) / kNodeW, dy = ((r + 0.5) * kNodeH - s.y) / kNodeH;
        v += s.amp * std::exp(-(dx * dx + dy * dy) / (2 * s.sigma * s.sigma));
      }
      put16(&p[64 + (r * 60 + c) * 2], uint16_t(1000 + int((r * 7 + c * 13 + counter * 5) % 9) - 4 + int(v)));
    }
  const uint32_t words = (payload - 20) / 4;
  uint32_t sum = 0;
  for (uint32_t i = 0; i < words * 2; ++i) sum += le16(&p[20 + i * 2]);
  const uint16_t check = uint16_t(~sum + 1);
  put16(&p[4], check);
  put32(&p[8], words);
  put16(&p[12], uint16_t(~check));
  put32(&p[16], ~words);
  return p;
}
static Spot finger(double x, double y, double amp = 600) { return Spot{x, y, amp, 0.8}; }
static Spot palmSpot(double x, double y) { return Spot{x, y, 500, 3.0}; }
static double nodeX(double c) { return (c + 0.5) * kNodeW; }
static double nodeY(double r) { return (r + 0.5) * kNodeH; }

// The frame loop's portable half on synthetic frames: matrix, tracker, the rotation, gestures, as PianoTouch::frame/gestures run
// them, a frame every 8 ms. Everything the gestures send is kept (and the frame it came in), the notes too.
struct Pipeline {
  std::unique_ptr<Matrix> m = std::make_unique<Matrix>();
  Tracker tr;
  Gesture g;
  uint16_t counter = 0;
  uint64_t now = 100000;
  uint32_t rotation = 0, frame = 0;
  struct Sent {
    PointerReport r;
    uint32_t frame;
    uint64_t ms;
  };
  std::vector<Sent> sent;
  std::vector<GestureNote> notes;
  std::vector<TrackNote> trackNotes;
  explicit Pipeline(TrackConfig tc = TrackConfig(), GestureConfig gc = GestureConfig(), MatrixConfig mc = MatrixConfig())
  {
    matrixReset(*m, mc);
    trackerReset(tr, tc);
    gestureReset(g, gc);
    for (unsigned n = 0; n < kProbeFrames + kRefFrames; ++n) {
      const std::vector<uint8_t> p = scenePacket(counter++, {});
      matrixFeed(*m, p.data(), uint32_t(p.size()));
    }
  }
  void take(const GestureOut &o)
  {
    for (unsigned i = 0; i < o.n; ++i) sent.push_back(Sent{o.r[i], frame, now});
    for (unsigned i = 0; i < o.noteCount; ++i) notes.push_back(o.notes[i]);
  }
  void step(const std::vector<Spot> &spots, uint64_t dtMs = 8)
  {
    now += dtMs;
    ++frame;
    const std::vector<uint8_t> p = scenePacket(counter++, spots);
    CHECK(matrixFeed(*m, p.data(), uint32_t(p.size())) == Feed::decoded);
    trackerUpdate(tr, m->points, m->pointCount, now);
    for (unsigned i = 0; i < tr.noteCount; ++i) trackNotes.push_back(tr.notes[i]);
    GestureContact c[kMaxTracks];
    unsigned n = 0;
    for (const Track &t : tr.t) {
      if (t.state != TrackState::active) continue;
      const PatPoint q = pat_rotate(rotation, t.x, t.y, kPanelW, kPanelH);
      c[n++] = GestureContact{t.id, t.palm, q.x, q.y, t.frames};
    }
    GestureOut o;
    gestureUpdate(g, c, n, now, o);
    take(o);
  }
  void hold(const std::vector<Spot> &spots, unsigned frames)
  {
    for (unsigned i = 0; i < frames; ++i) step(spots);
  }
  void none(unsigned frames = 6) { hold({}, frames); }
  void lose()
  {
    trackerCancel(tr);
    GestureOut o;
    gestureCancel(g, o, now);
    take(o);
  }
  bool any(uint8_t buttonBits) const
  {
    for (const Sent &s : sent)
      if (s.r.buttons & buttonBits) return true;
    return false;
  }
  unsigned noted(GestureKind k) const
  {
    unsigned n = 0;
    for (const GestureNote &x : notes) n += x.kind == k;
    return n;
  }
  int32_t wheel() const
  {
    int32_t w = 0;
    for (const Sent &s : sent) w += s.r.wheel;
    return w;
  }
  int32_t pan() const
  {
    int32_t w = 0;
    for (const Sent &s : sent) w += s.r.pan;
    return w;
  }
  // Every press is released, every modifier let go, by the end.
  bool clean() const { return sent.empty() || (sent.back().r.buttons == 0 && sent.back().r.modifiers == 0); }
  // The clicks: each press (after a report without one) released at once at the same place.
  std::vector<PointerReport> clicks() const
  {
    std::vector<PointerReport> out;
    for (size_t i = 0; i + 1 < sent.size(); ++i)
      if (sent[i].r.buttons && !sent[i + 1].r.buttons && sent[i].r.x == sent[i + 1].r.x && sent[i].r.y == sent[i + 1].r.y &&
          (i == 0 || !sent[i - 1].r.buttons))
        out.push_back(sent[i].r);
    return out;
  }
};
static bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }
// Positions: the 3 x 3 centroid of the linear map lands within about 8 px (0.6 mm) of a Gaussian's centre.
constexpr double kPx = 12;

// The matrix's points: two fingers apart are two points, a ridge with no dip one, a palm one large blob with its peaks counted, a
// weak peak kept (below the threshold, above the low level) but not strong; the single contact unchanged among them.
static void testPoints()
{
  Pipeline p;
  Matrix &m = *p.m;
  auto feed = [&](const std::vector<Spot> &spots) {
    const std::vector<uint8_t> pk = scenePacket(p.counter++, spots);
    return matrixFeed(m, pk.data(), uint32_t(pk.size())) == Feed::decoded;
  };
  CHECK(feed({finger(nodeX(20), nodeY(10)), finger(nodeX(40), nodeY(25), 700)}));
  CHECK(m.pointCount == 2 && m.points[0].strong && m.points[1].strong && m.peaks == 2);
  CHECK(m.points[0].peak > m.points[1].peak && near(m.points[0].x, nodeX(40), 3) && near(m.points[0].y, nodeY(25), 3));
  CHECK(m.points[0].area >= 5 && m.points[0].area <= 16 && m.points[0].blobPeaks == 1 && m.touching);
  CHECK(near(m.x, nodeX(40), 3));   // the single contact: the strongest
  // Two fingers three nodes apart (fingertips side by side): still two, neither pulled toward the other by the widened window.
  CHECK(feed({finger(nodeX(20), nodeY(20)), finger(nodeX(23), nodeY(20))}) && m.pointCount == 2);
  for (uint32_t i = 0; i < m.pointCount; ++i)
    CHECK(near(m.points[i].x, nodeX(20), kPx) || near(m.points[i].x, nodeX(23), kPx));
  // A finger resting exactly between two electrodes (its peak node flickers between them with the noise): one position, steady.
  {
    int32_t lo = INT32_MAX, hi = INT32_MIN, ylo = INT32_MAX, yhi = INT32_MIN;
    for (int k = 0; k < 40; ++k) {
      CHECK(feed({finger(nodeX(29.5), nodeY(19.5))}) && m.pointCount == 1);
      lo = std::min(lo, m.points[0].x);
      hi = std::max(hi, m.points[0].x);
      ylo = std::min(ylo, m.points[0].y);
      yhi = std::max(yhi, m.points[0].y);
    }
    CHECK(hi - lo <= 3 && yhi - ylo <= 3 && near(lo, nodeX(29.5), kPx) && near(ylo, nodeY(19.5), kPx));
  }
  // The widened window across a sweep: the position follows the finger monotonically, within the tolerance everywhere.
  {
    int32_t last = -1;
    bool monotonic = true, close = true;
    for (double x = nodeX(20); x <= nodeX(24); x += 2) {
      CHECK(feed({finger(x, nodeY(10.3))}) && m.pointCount == 1);
      monotonic = monotonic && m.points[0].x >= last - 2;
      close = close && near(m.points[0].x, x, kPx);
      last = m.points[0].x;
    }
    CHECK(monotonic && close);
  }
  // A ridge: two maxima two nodes apart with the node between almost as high: one contact (merged).
  const uint32_t mergedBefore = m.merged;
  CHECK(feed({Spot{nodeX(30), nodeY(20), 500, 0.6}, Spot{nodeX(31), nodeY(20), 260, 0.6}, Spot{nodeX(32), nodeY(20), 520, 0.6}}));
  CHECK(m.pointCount == 1 && m.merged == mergedBefore + 1);
  // A palm: one blob of many nodes; a hand's ridges (three bumps on it) count as its peaks.
  CHECK(feed({palmSpot(nodeX(15), nodeY(20))}) && m.pointCount >= 1 && m.points[0].area >= 48);
  CHECK(feed({palmSpot(nodeX(15), nodeY(20)), Spot{nodeX(11), nodeY(17), 250, 0.7}, Spot{nodeX(19), nodeY(17), 250, 0.7},
              Spot{nodeX(15), nodeY(25), 250, 0.7}}));
  CHECK(m.pointCount >= 1 && m.points[0].blobPeaks >= 3 && m.pointCount <= kBlobPoints);
  // A weak peak (above 60 % of the threshold, below it): a point, not strong, and no single contact.
  CHECK(feed({finger(nodeX(30), nodeY(30), 150)}) && m.pointCount == 1 && !m.points[0].strong && !m.touching && m.peaks == 0);
  CHECK(feed({finger(nodeX(30), nodeY(30), 90)}) && m.pointCount == 0);
  // A palm's peaks cannot crowd out a finger: kBlobPoints a blob at most, the finger still listed.
  std::vector<Spot> crowd = {palmSpot(nodeX(15), nodeY(20)), finger(nodeX(50), nodeY(10), 300)};
  for (int k = 0; k < 12; ++k) crowd.push_back(Spot{nodeX(10 + (k % 4) * 3), nodeY(15 + (k / 4) * 4), 300, 0.6});
  CHECK(feed(crowd));
  bool fingerListed = false;
  for (uint32_t i = 0; i < m.pointCount; ++i) fingerListed = fingerListed || (near(m.points[i].x, nodeX(50), kPx) && m.points[i].area < 30);
  CHECK(fingerListed && m.pointCount <= 1 + kBlobPoints);
}

// The tracker alone: stable IDs while fingers move and pass each other, a dropout under `hold` frames kept, the hysteresis, the
// first-frame confirmation of a strong peak, palms (by size, by peaks, a finger that flattens, the remnant of a lifting palm).
static void testTracker()
{
  {   // two fingers crossing in X on rows 200 px apart: each keeps its ID
    Pipeline p;
    p.step({finger(800, 900), finger(2400, 1100)});
    CHECK(trackerActive(p.tr, true) == 2);
    uint8_t left = 0, right = 0;
    for (const Track &t : p.tr.t)
      if (t.state == TrackState::active) (t.x < 1600 ? left : right) = t.id;
    for (int k = 1; k <= 40; ++k) p.step({finger(800 + 40 * k, 900), finger(2400 - 40 * k, 1100)});
    bool same = true;
    for (const Track &t : p.tr.t)
      if (t.state == TrackState::active) same = same && (t.y < 1000 ? t.id == left && t.x > 2300 : t.id == right && t.x < 900);
    CHECK(same && trackerActive(p.tr, true) == 2 && p.tr.downs == 2 && p.tr.ups == 0);
  }
  {   // a dropout: two frames missing keep the ID, three end the contact
    Pipeline p;
    p.hold({finger(1000, 1000)}, 5);
    const uint8_t id = p.tr.t[0].id;
    p.none(2);
    CHECK(trackerActive(p.tr, false) == 1);
    p.hold({finger(1010, 1000)}, 3);
    CHECK(p.tr.t[0].id == id && p.tr.downs == 1 && p.tr.ups == 0);
    p.none(3);
    CHECK(trackerLive(p.tr) == 0 && p.tr.ups == 1);
    p.hold({finger(1010, 1000)}, 2);
    CHECK(p.tr.downs == 2 && p.tr.t[0].id != id);
  }
  {   // frames 200 ms apart (no edge and the old fixed fallback): the lift is seen at the first frame without the contact, not 3
    Pipeline p;
    p.step({finger(1000, 1000)}, 200);
    p.step({finger(1000, 1000)}, 200);
    p.step({}, 200);
    CHECK(trackerLive(p.tr) == 0 && p.tr.ups == 1 && p.clicks().size() == 1);
  }
  {   // hysteresis: a contact fading to below the threshold continues; the same weak blob alone starts nothing
    Pipeline p;
    p.hold({finger(1500, 700)}, 3);
    for (int a = 600; a >= 140; a -= 40) p.step({finger(1500, 700, a)});
    CHECK(trackerActive(p.tr, false) == 1 && p.tr.downs == 1);
    p.none(3);
    p.hold({finger(1500, 700, 150)}, 10);
    CHECK(trackerLive(p.tr) == 0 && p.tr.downs == 1);
  }
  {   // confirmation: a strong first peak counts in its first frame, a weaker one in its second; one weak frame is nothing
    Pipeline p;
    p.step({finger(600, 600, 600)});
    CHECK(trackerActive(p.tr, false) == 1);
    p.none(3);
    p.step({finger(600, 600, 300)});
    CHECK(trackerActive(p.tr, false) == 0 && trackerLive(p.tr) == 1);
    p.step({});
    CHECK(trackerLive(p.tr) == 0 && p.tr.downs == 1);
    p.step({finger(600, 600, 300)});
    p.step({finger(600, 600, 300)});
    CHECK(trackerActive(p.tr, false) == 1 && p.tr.downs == 2);
  }
  {   // palms: a large blob never starts a contact; neither do its peaks
    Pipeline p;
    p.hold({palmSpot(1200, 1200)}, 20);
    CHECK(trackerActive(p.tr, true) == 0 && p.tr.palmPoints > 0 && p.tr.downs == 0);
    // its remnant as it lifts (a fingertip-sized blob where it was) is the palm's, for as long as it stays
    p.hold({finger(1250, 1150)}, 80);
    CHECK(trackerActive(p.tr, true) == 0 && trackerActive(p.tr, false) == 1 && p.tr.palms == 1);
    p.none(3);
    // 500 ms later the place is free again: a finger there is a finger
    p.none(70);
    p.hold({finger(1250, 1150)}, 3);
    CHECK(trackerActive(p.tr, true) == 1);
  }
  {   // a finger that flattens into a palm is a palm until it lifts; a palm away from a finger leaves the finger alone
    Pipeline p;
    p.hold({finger(1000, 1000)}, 3);
    p.hold({palmSpot(1000, 1000)}, 3);
    p.hold({finger(1000, 1000)}, 3);
    CHECK(trackerActive(p.tr, false) == 1 && trackerActive(p.tr, true) == 0 && p.tr.palms == 1);
    Pipeline q;
    q.hold({palmSpot(900, 1300), finger(2500, 500)}, 10);
    CHECK(trackerActive(q.tr, true) == 1);
  }
  {   // palm by peaks: three bumps in one blob of modest size
    TrackConfig tc;
    tc.palmNodes = 0;   // size off: the peaks alone decide
    Pipeline p(tc);
    p.hold({Spot{nodeX(20), nodeY(20), 450, 1.3}, Spot{nodeX(17), nodeY(20), 450, 1.0}, Spot{nodeX(23), nodeY(20), 450, 1.0}}, 5);
    CHECK(p.m->pointCount >= 1 && p.m->points[0].blobPeaks >= 3 && trackerActive(p.tr, true) == 0);
  }
}

// The gestures on synthetic frames, end to end (matrix, tracker, gestures).
static void testGestures()
{
  const double X = 1100, Y = 900;
  {   // tap: the pointer there in the contact's first frame (a strong peak), the click at the lift, where it landed
    Pipeline p;
    p.hold({finger(X, Y)}, 10);
    CHECK(p.sent.size() == 1 && p.sent[0].frame == 1 && p.sent[0].r.buttons == 0);
    CHECK(near(p.sent[0].r.x, X, kPx) && near(p.sent[0].r.y, Y, kPx));
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonLeft && c[0].x == p.sent[0].r.x && c[0].y == p.sent[0].r.y);
    CHECK(p.sent.size() == 3 && p.noted(GestureKind::tap) == 1 && p.clean() && p.g.mode == GestureMode::idle);
    CHECK(p.sent.size() == 3 && p.sent[1].frame == 13);   // the lift is seen after `hold` (3) frames without the contact
    if (p.sent.size() == 3)
      std::printf("gestures: tap -> pointer in the contact's first frame, click %llu ms after the lift (8 ms frames)\n",
                  (unsigned long long)(p.sent[1].ms - p.sent[0].ms - 9 * 8));
  }
  {   // at the blind busy rate (a frame per 12 ms, no edges): the same tap, the drag and the scroll
    Pipeline p;
    p.step({finger(X, Y)}, 12);
    for (int k = 0; k < 8; ++k) p.step({finger(X, Y)}, 12);
    for (int k = 0; k < 4; ++k) p.step({}, 12);
    CHECK(p.clicks().size() == 1 && p.sent[0].frame == 1);
    for (int k = 1; k <= 30; ++k) p.step({finger(X + 15 * k, Y)}, 12);
    for (int k = 0; k < 4; ++k) p.step({}, 12);
    CHECK(p.noted(GestureKind::dragEnd) == 1 && p.clean());
  }
  {   // the first report one frame later without the fast confirmation; a tap all the same
    TrackConfig tc;
    tc.fastPeak = 0;
    Pipeline p(tc);
    p.hold({finger(X, Y)}, 10);
    CHECK(!p.sent.empty() && p.sent[0].frame == 2);
    p.none();
    CHECK(p.clicks().size() == 1);
  }
  {   // a resting finger moving by a pixel or so sends nothing between the pointer move and the click (no jitter)
    Pipeline p;
    for (int k = 0; k < 38; ++k) p.step({finger(X + (k % 3) - 1, Y + (k % 2))});
    CHECK(p.sent.size() == 1);
    p.none();
    CHECK(p.clicks().size() == 1 && p.sent.size() == 3);
  }
  {   // hold: a right click at 500 ms where it landed, nothing at the lift, never a left button
    Pipeline p;
    p.hold({finger(X, Y)}, 80);
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonRight && !p.any(kButtonLeft) && p.noted(GestureKind::holdRight) == 1);
    unsigned at = 0;
    while (at < p.sent.size() && !p.sent[at].r.buttons) ++at;
    CHECK(at < p.sent.size() && p.sent[at].ms - 100008 >= 500 && p.sent[at].ms - 100008 <= 510);
    if (at < p.sent.size()) std::printf("gestures: hold -> right click %llu ms after the landing\n", (unsigned long long)(p.sent[at].ms - 100008));
    CHECK(p.clean() && p.sent.size() == 3);
  }
  {   // hold, then moving: the right click, then the pointer follows without a button
    Pipeline p;
    p.hold({finger(X, Y)}, 70);
    for (int k = 1; k <= 20; ++k) p.step({finger(X + 15 * k, Y)});
    p.none();
    CHECK(p.clicks().size() == 1 && !p.any(kButtonLeft) && p.sent.back().r.x > X + 200 && p.clean());
  }
  {   // drag: pressed where it landed, following the finger, released at the lift; never a right click even past 500 ms
    Pipeline p;
    p.hold({finger(800, 1000)}, 2);
    for (int k = 1; k <= 80; ++k) p.step({finger(800 + 10 * k, 1000)});
    p.hold({finger(1600, 1000)}, 5);
    p.none();
    CHECK(!p.any(kButtonRight) && p.noted(GestureKind::dragStart) == 1 && p.noted(GestureKind::dragEnd) == 1 && p.clean());
    unsigned first = 0;
    while (first < p.sent.size() && !p.sent[first].r.buttons) ++first;
    CHECK(first < p.sent.size() && p.sent[first].r.buttons == kButtonLeft && p.sent[first].r.x == p.sent[0].r.x);   // at the landing
    bool forward = true;
    for (size_t i = first + 1; i + 1 < p.sent.size(); ++i)
      forward = forward && p.sent[i].r.buttons == kButtonLeft && p.sent[i].r.x >= p.sent[i - 1].r.x;
    CHECK(forward && near(p.sent.back().r.x, 1600, kPx) && p.sent.back().r.buttons == 0);
  }
  {   // a second finger during a drag changes nothing: the drag goes on, no scroll (the drag started: past the slop 80 ms)
    Pipeline p;
    p.hold({finger(800, 1000)}, 2);
    for (int k = 1; k <= 20; ++k) p.step({finger(800 + 10 * k, 1000)});
    CHECK(p.g.buttons == kButtonLeft);
    for (int k = 21; k <= 40; ++k) p.step({finger(800 + 10 * k, 1000), finger(2000, 600 - 5 * k)});
    p.none();
    CHECK(p.wheel() == 0 && p.noted(GestureKind::dragEnd) == 1 && !p.any(kButtonRight) && p.clean());
  }
  {   // two-finger tap: the pointer between them, a right click there, no left click
    Pipeline p;
    p.hold({finger(1200, 1000), finger(1500, 1000)}, 12);
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonRight && near(c[0].x, 1350, kPx) && near(c[0].y, 1000, kPx));
    CHECK(!p.any(kButtonLeft) && p.noted(GestureKind::twoTap) == 1 && p.clean());
  }
  {   // the second finger 80 ms after the first, the first lifting a little before the second: still a two-finger tap
    Pipeline p;
    p.hold({finger(1200, 1000)}, 10);
    p.hold({finger(1200, 1000), finger(1500, 1000)}, 10);
    p.hold({finger(1500, 1000)}, 2);
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonRight && !p.any(kButtonLeft));
  }
  {   // two fingers held long and lifted: nothing
    Pipeline p;
    p.hold({finger(1200, 1000), finger(1500, 1000)}, 100);
    p.none();
    CHECK(p.clicks().empty() && p.clean());
  }
  // 2026-11-12 (attend1): a lifting finger's centre moves 30-70 px in its last frames; short taps became drags with a 32 px slop.
  {   // a tap whose centre drifts 50 px as it lifts: still a tap at the landing (64 px slop)
    Pipeline p;
    p.hold({finger(X, Y)}, 10);
    p.step({finger(X + 25, Y + 10)});
    p.step({finger(X + 50, Y + 20)});
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonLeft && near(c[0].x, X, kPx) && near(c[0].y, Y, kPx));
    CHECK(p.noted(GestureKind::dragStart) == 0 && p.noted(GestureKind::tap) == 1 && p.clean());
  }
  {   // a drift past the slop (110 px) in the last frame before the lift: the drag waits 80 ms, the lift makes it a tap
    Pipeline p;
    p.hold({finger(X, Y)}, 10);
    p.step({finger(X + 110, Y)});
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonLeft && near(c[0].x, X, kPx) && p.noted(GestureKind::dragStart) == 0);
    bool past = false;
    for (const GestureNote &n : p.notes) past = past || (n.kind == GestureKind::tap && n.b == 1 && n.a > 64);
    CHECK(past && p.clean());
  }
  {   // a slow drag: it starts once past the slop for 80 ms, pressed where the finger landed, then follows
    Pipeline p;
    p.hold({finger(800, 1000)}, 2);
    for (int k = 1; k <= 40; ++k) p.step({finger(800 + 5 * k, 1000)});
    p.none();
    unsigned first = 0;
    while (first < p.sent.size() && !p.sent[first].r.buttons) ++first;
    CHECK(first < p.sent.size() && p.sent[first].r.x == p.sent[0].r.x && p.noted(GestureKind::dragStart) == 1);
    // past 64 px at about frame 13 (5 px a frame from frame 3); the press 10 frames (80 ms) later
    CHECK(first < p.sent.size() && p.sent[first].frame >= 20 && p.sent[first].frame <= 30);
    if (first < p.sent.size()) std::printf("gestures: a 5 px a frame drag pressed in frame %u (past the slop about frame 15)\n", p.sent[first].frame);
    CHECK(near(p.sent.back().r.x, 1000, kPx) && p.noted(GestureKind::dragEnd) == 1 && p.clean() && p.clicks().empty());
  }
  {   // a fast move past three times the slop starts the drag at once
    Pipeline p;
    p.hold({finger(800, 1000)}, 3);
    for (int k = 1; k <= 6; ++k) p.step({finger(800 + 60 * k, 1000)});
    p.none();
    unsigned first = 0;
    while (first < p.sent.size() && !p.sent[first].r.buttons) ++first;
    CHECK(first < p.sent.size() && p.sent[first].frame <= 8 && p.noted(GestureKind::dragStart) == 1 && p.clean());
  }
  {   // piano-touch-drag-delay=0: the drag at the first frame past the slop, as before
    GestureConfig gc;
    gc.dragDelayMs = 0;
    Pipeline p(TrackConfig(), gc);
    p.hold({finger(X, Y)}, 10);
    p.step({finger(X + 110, Y)});
    p.none();
    CHECK(p.noted(GestureKind::dragStart) == 1 && p.clicks().empty() && p.clean());
  }
  {   // a finger held still but wobbling 40 px: the right click at 500 ms, never a drag
    Pipeline p;
    for (int k = 0; k < 75; ++k) p.step({finger(X + ((k / 4) % 2 ? 40 : 0), Y)});
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonRight && !p.any(kButtonLeft) && p.noted(GestureKind::dragStart) == 0);
  }
  {   // a two-finger tap of 560 ms (attend1's 517-526 ms ones were lost with 500): a right click
    Pipeline p;
    p.hold({finger(1200, 1000), finger(1500, 1000)}, 70);
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonRight && p.noted(GestureKind::twoTap) == 1);
  }
  auto scroll = [](double dx, double dy, GestureConfig gc) {
    Pipeline p(TrackConfig(), gc);
    p.hold({finger(1300, 1300), finger(1600, 1300)}, 3);
    for (int k = 1; k <= 50; ++k) p.step({finger(1300 + dx * k / 50, 1300 + dy * k / 50), finger(1600 + dx * k / 50, 1300 + dy * k / 50)});
    p.none();
    return p;
  };
  {   // two fingers up 400 px: wheel up by 400 / 4 (the content follows the fingers under natural scrolling), no pan, no button,
      // the pointer fixed between the fingers where they started
    const Pipeline p = scroll(0, -400, GestureConfig());
    CHECK(p.wheel() >= 96 && p.wheel() <= 101 && p.pan() == 0 && !p.any(7) && p.clean());
    std::printf("gestures: two fingers up 400 px -> wheel %+d, AC Pan %+d in %zu reports, pointer fixed between them\n", p.wheel(), p.pan(),
                p.sent.size() - 2);
    CHECK(p.noted(GestureKind::scrollStart) == 1 && p.noted(GestureKind::scrollEnd) == 1 && p.clicks().empty());
    bool fixed = true;
    for (size_t i = 1; i < p.sent.size(); ++i) fixed = fixed && p.sent[i].r.x == p.sent[1].r.x && p.sent[i].r.y == p.sent[1].r.y;
    CHECK(p.sent.size() > 2 && fixed && near(p.sent[1].r.x, 1450, kPx) && near(p.sent[1].r.y, 1300, kPx));
    bool small = true;
    for (size_t i = 3; i < p.sent.size(); ++i) small = small && p.sent[i].r.wheel >= 0 && p.sent[i].r.wheel <= 6;   // a few counts a frame
    CHECK(small);
  }
  {   // down, right, left; the axis lock; a diagonal both; inverted; a coarser count
    CHECK(scroll(0, 400, GestureConfig()).wheel() <= -96);
    const Pipeline r = scroll(400, 30, GestureConfig());
    CHECK(r.pan() >= 96 && r.wheel() == 0);
    CHECK(scroll(-400, 0, GestureConfig()).pan() <= -96);
    const Pipeline d = scroll(300, -300, GestureConfig());
    CHECK(d.pan() >= 70 && d.wheel() >= 70);
    GestureConfig inv;
    inv.scrollInvert = true;
    CHECK(scroll(0, -400, inv).wheel() <= -96);
    GestureConfig coarse;
    coarse.scrollPx = 10;
    CHECK(near(scroll(0, -400, coarse).wheel(), 40, 2));
  }
  {   // pinch: by default recognised and nothing sent (no wheel, no button, no modifier)
    auto pinch = [](GestureConfig gc, double from, double to) {
      Pipeline p(TrackConfig(), gc);
      p.hold({finger(1600 - from / 2, 1000), finger(1600 + from / 2, 1000)}, 3);
      for (int k = 1; k <= 40; ++k) {
        const double s = from + (to - from) * k / 40;
        p.step({finger(1600 - s / 2, 1000), finger(1600 + s / 2, 1000)});
      }
      p.none();
      return p;
    };
    const Pipeline skip = pinch(GestureConfig(), 300, 900);
    CHECK(skip.wheel() == 0 && !skip.any(7) && skip.noted(GestureKind::pinchStart) == 1 && skip.noted(GestureKind::pinchEnd) == 1);
    bool noModifier = true;
    for (const auto &s : skip.sent) noModifier = noModifier && !s.r.modifiers;
    CHECK(noModifier && skip.noted(GestureKind::scrollStart) == 0);
    GestureConfig cmd;
    cmd.pinch = 1;
    const Pipeline in = pinch(cmd, 300, 900);
    bool held = true;
    for (const auto &s : in.sent) held = held && (!s.r.wheel || s.r.modifiers == kModifierCommand);
    CHECK(held && in.wheel() >= 34 && in.wheel() <= 38 && in.clean() && in.sent.back().r.modifiers == 0);
    GestureConfig ctrl;
    ctrl.pinch = 2;
    const Pipeline out = pinch(ctrl, 900, 300);
    CHECK(out.wheel() <= -34 && out.clean());
    for (const auto &s : out.sent) CHECK(!s.r.wheel || s.r.modifiers == kModifierControl);
  }
  {   // three fingers: nothing after the first pointer move
    Pipeline p;
    p.hold({finger(1000, 800), finger(1300, 800), finger(1600, 800)}, 10);
    for (int k = 1; k <= 20; ++k) p.step({finger(1000, 800 - 10 * k), finger(1300, 800 - 10 * k), finger(1600, 800 - 10 * k)});
    p.none();
    CHECK(p.sent.size() <= 1 && p.clicks().empty() && p.wheel() == 0);
  }
  {   // a palm alone: nothing at all; a palm resting while a finger taps elsewhere: the tap clicks
    Pipeline p;
    p.hold({palmSpot(800, 1500)}, 30);
    p.none();
    CHECK(p.sent.empty());
    Pipeline q;
    q.hold({palmSpot(800, 1500)}, 10);
    q.hold({palmSpot(800, 1500), finger(2400, 600)}, 10);
    q.hold({palmSpot(800, 1500)}, 10);
    q.none();
    const std::vector<PointerReport> c = q.clicks();
    CHECK(c.size() == 1 && c[0].buttons == kButtonLeft && near(c[0].x, 2400, kPx));
  }
  {   // a palm landing (small, then large) and lifting (large, then small) where it is: no click
    Pipeline p;
    for (double sg = 0.8; sg < 3.0; sg += 0.4) p.step({Spot{1500, 1200, 500, sg}});
    p.hold({palmSpot(1500, 1200)}, 20);
    for (double sg = 3.0; sg > 0.7; sg -= 0.4) p.step({Spot{1500, 1200, 500, sg}});
    p.hold({finger(1520, 1180, 400)}, 4);
    p.none();
    CHECK(p.clicks().empty() && !p.any(7) && p.clean());
    CHECK(p.noted(GestureKind::cancelPalm) <= 1);
  }
  {   // noise: one strong frame moves the pointer at most, never clicks; one frame at the threshold sends nothing
    Pipeline p;
    p.step({finger(X, Y, 900)});
    p.none();
    CHECK(p.clicks().empty() && p.sent.size() <= 1);
    Pipeline q;
    q.step({finger(X, Y, 300)});
    q.none();
    CHECK(q.sent.empty());
  }
  {   // a double tap: the second tap near the first (within 48 px, 500 ms) clicks exactly where the first did; later, its own place
    Pipeline p;
    p.hold({finger(X, Y)}, 8);
    p.none();
    p.hold({finger(X + 20, Y + 12)}, 8);
    p.none();
    std::vector<PointerReport> c = p.clicks();
    CHECK(c.size() == 2 && c[0].x == c[1].x && c[0].y == c[1].y);
    p.none(70);
    p.hold({finger(X + 20, Y + 12)}, 8);
    p.none();
    c = p.clicks();
    CHECK(c.size() == 3 && c[2].x != c[0].x);
    Pipeline q;   // farther than 48 px: its own place
    q.hold({finger(X, Y)}, 8);
    q.none();
    q.hold({finger(X + 100, Y)}, 8);
    q.none();
    c = q.clicks();
    CHECK(c.size() == 2 && near(c[1].x, X + 100, kPx));
  }
  {   // frames lost during a drag: released at once; during a pinch with Command: Command released
    Pipeline p;
    p.hold({finger(800, 1000)}, 2);
    for (int k = 1; k <= 20; ++k) p.step({finger(800 + 10 * k, 1000)});
    CHECK(p.g.buttons == kButtonLeft);
    p.lose();
    CHECK(p.clean() && p.noted(GestureKind::cancelLost) == 1 && p.g.mode == GestureMode::idle && trackerLive(p.tr) == 0);
    GestureConfig cmd;
    cmd.pinch = 1;
    Pipeline q(TrackConfig(), cmd);
    q.hold({finger(1400, 1000), finger(1700, 1000)}, 3);
    for (int k = 1; k <= 10; ++k) q.step({finger(1400 - 10 * k, 1000), finger(1700 + 10 * k, 1000)});
    CHECK(q.g.modifiers == kModifierCommand);
    q.lose();
    CHECK(q.clean() && q.g.modifiers == 0 && q.noted(GestureKind::pinchEnd) == 1);
  }
  {   // the plain pointer of before (piano-touch-gestures=0): down presses, moves drag, up releases
    GestureConfig plain;
    plain.gestures = false;
    Pipeline p(TrackConfig(), plain);
    p.hold({finger(X, Y)}, 3);
    CHECK(p.sent.size() == 2 && p.sent[0].r.buttons == 0 && p.sent[1].r.buttons == kButtonLeft);
    for (int k = 1; k <= 5; ++k) p.step({finger(X + 20 * k, Y)});
    p.none();
    CHECK(p.clean() && p.noted(GestureKind::plainUp) == 1 && near(p.sent.back().r.x, X + 100, kPx));
  }
  {   // the corners: positions stay on the panel, the HID values reach both ends
    Pipeline p;
    p.hold({finger(nodeX(0), nodeY(0))}, 8);
    p.none();
    Pipeline q;
    q.hold({finger(nodeX(59), nodeY(39))}, 8);
    q.none();
    CHECK(p.clicks().size() == 1 && q.clicks().size() == 1);
    if (p.clicks().size() == 1 && q.clicks().size() == 1) {
      uint8_t r[kMaxReportBytes];
      pointerReport(p.clicks()[0], kPanelW, kPanelH, false, r);
      CHECK(le16(r + 1) < 600 && le16(r + 3) < 900);
      pointerReport(q.clicks()[0], kPanelW, kPanelH, false, r);
      CHECK(le16(r + 1) > kHidMax - 600 && le16(r + 3) > kHidMax - 900);
    }
  }
}

// piano-touch-rotate=1 with the gestures: the contacts mapped into the rotated desktop before the gestures, so a tap lands under the
// finger and a scroll follows the desktop's directions (CGDisplayRotation 90: the panel's left edge up).
static void testGestureRotation()
{
  for (uint32_t deg : {0u, 90u, 180u, 270u}) {
    Pipeline p;
    p.rotation = deg;
    p.hold({finger(700, 500)}, 8);
    p.none();
    const std::vector<PointerReport> c = p.clicks();
    const PatPoint want = pat_rotate(deg, 700, 500, kPanelW, kPanelH);
    CHECK(c.size() == 1 && near(c[0].x, want.x, kPx) && near(c[0].y, want.y, kPx));
    if (c.size() == 1) {   // the bytes: scaled over the rotated desktop (h x w at 90 and 270)
      uint8_t r[kMaxReportBytes];
      pointerReport(c[0], want.width, want.height, false, r);
      CHECK(near(le16(r + 1), double(want.x) * kHidMax / (want.width - 1), 60) &&
            near(le16(r + 3), double(want.y) * kHidMax / (want.height - 1), 60));
    }
  }
  // At 90 the desktop's up is the panel's left edge: fingers moving toward it (panel x falling) scroll up; toward the panel's top
  // edge (panel y falling) is the desktop's right: AC Pan right.
  auto swipe = [](uint32_t deg, double dx, double dy) {
    Pipeline p;
    p.rotation = deg;
    p.hold({finger(1500, 900), finger(1500, 1200)}, 3);
    for (int k = 1; k <= 40; ++k) p.step({finger(1500 + dx * k / 40, 900 + dy * k / 40), finger(1500 + dx * k / 40, 1200 + dy * k / 40)});
    p.none();
    return std::pair<int32_t, int32_t>(p.wheel(), p.pan());
  };
  auto w = swipe(90, -320, 0);
  CHECK(w.first >= 76 && w.second == 0);
  w = swipe(90, 0, -320);
  CHECK(w.second >= 76 && w.first == 0);
  w = swipe(270, -320, 0);
  CHECK(w.first <= -76 && w.second == 0);
  w = swipe(180, 0, -320);
  CHECK(w.first <= -76);
  w = swipe(0, 0, -320);
  CHECK(w.first >= 76);
  // A drag at 90: it follows the finger in desktop coordinates (panel y falling = desktop x rising).
  Pipeline p;
  p.rotation = 90;
  p.hold({finger(1000, 1500)}, 2);
  for (int k = 1; k <= 30; ++k) p.step({finger(1000, 1500 - 20 * k)});
  p.none();
  CHECK(p.noted(GestureKind::dragEnd) == 1 && p.sent.back().r.x > p.sent[0].r.x + 500 && near(p.sent.back().r.y, p.sent[0].r.y, 8));
}

// The power states (TouchPower.h): the display off mutes (frames still read, slowly), the system off suspends (no frames) from
// either, resume returns to what the display is; an unknown display counts as on; piano-touch-display=0 ignores it. The restart
// plan: kept, go on (after a suspend with the reference learned again); lost, at most two downloads, then give up.
static void testPower()
{
  TouchPower p;
  CHECK(p.step(false, -1) == PowerStep::none && p.reporting() && p.reading());
  CHECK(p.step(false, 1) == PowerStep::none);
  CHECK(p.step(false, 0) == PowerStep::mute && p.muted() && !p.reporting() && p.reading());
  CHECK(p.step(false, 0) == PowerStep::none && p.muted());
  CHECK(p.step(true, 0) == PowerStep::suspend && !p.reading() && !p.reporting());
  CHECK(p.step(true, 1) == PowerStep::none && !p.reading());
  CHECK(p.step(false, 0) == PowerStep::resume && p.muted());       // back into a dark display: still muted
  CHECK(p.step(false, -1) == PowerStep::unmute && p.reporting());  // not published any more: on
  CHECK(p.step(true, 1) == PowerStep::suspend && p.step(false, 1) == PowerStep::resume && p.reporting());
  CHECK(p.suspends == 2 && p.resumes == 2 && p.mutes == 1 && p.unmutes == 1);
  TouchPower q;
  q.followDisplay = false;
  CHECK(q.step(false, 0) == PowerStep::none && q.reporting() && q.step(true, 0) == PowerStep::suspend && q.step(false, 0) == PowerStep::resume &&
        q.reporting());
  CHECK(restartPlan(true, true, 0) == Restart::relearn && restartPlan(false, true, 0) == Restart::keep);
  CHECK(restartPlan(true, false, 0) == Restart::download && restartPlan(false, false, 1) == Restart::download);
  CHECK(restartPlan(true, false, kResumeDownloads) == Restart::giveUp);
  // The gestures across a mute: a drag in progress is released, the next touch after the display's return starts afresh.
  Pipeline g;
  g.hold({finger(800, 1000)}, 2);
  for (int k = 1; k <= 20; ++k) g.step({finger(800 + 10 * k, 1000)});
  CHECK(g.g.buttons == kButtonLeft);
  g.lose();   // PianoTouch::lost at the mute: tracker and gestures cancelled
  CHECK(g.clean() && g.g.buttons == 0);
  trackerReset(g.tr, TrackConfig());
  gestureReset(g.g, GestureConfig());
  g.sent.clear();
  g.hold({finger(1200, 700)}, 8);
  g.none();
  CHECK(g.clicks().size() == 1);
}

// The CPU latency hold of a read burst: taken at the first busy pass, kept through the burst and 1.5 s after, then released.
static void testBurstHold()
{
  BurstHold h;
  CHECK(h.step(1000, false) == 0 && !h.held);
  CHECK(h.step(1001, true) == 1 && h.held && h.takes == 1);
  CHECK(h.step(1009, true) == 0 && h.step(1500, false) == 0 && h.held);
  CHECK(h.step(1009 + kHoldMs - 1, false) == 0 && h.step(1009 + kHoldMs, false) == -1 && !h.held);
  CHECK(h.step(5000, false) == 0 && h.step(5001, true) == 1 && h.takes == 2);
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

// The frame pace against a model of the NT36532E's frame handshake as the first level-2 runs on the tablet show it (TouchPace.h):
// a frame is ready kScan us after the read of the previous one ended; it raises the line (a rising edge the host's TLMM latch keeps,
// unless the test loses it) and waits. A read that starts before it is ready, or a frame left unread for kUnreadLimit us, puts the
// controller into its watchdog state (transport 0xfd: no frames, the line low) until a download. Both triggers are the hypotheses
// the runs agree with: attend1's display-off pace (reads ~500-560 ms apart) went to 0xfd while muted, the level-1 loop (200 ms
// apart at most) never did, so the unread limit is modeled between them at 450 ms; attend2's three downloads went to 0xfd at once
// while the 12 ms edge-less reads ran inside a ~12.6 ms cycle.
struct FeedModel {
  uint64_t scanUs = 6500, unreadLimitUs = 450000;
  uint64_t startUs = 0;   // the first frame after a download's start comes this much later than a scan (the tablet: ~850 ms)
  uint64_t readyAt = 6500;
  bool edgeOut = false, watchdog = false;
  uint32_t collisions = 0, starvations = 0, downloads = 0;
  bool pending(uint64_t t) const { return !watchdog && t >= readyAt; }
  // A read from t for readUs: the frame (false: a watchdog frame, or a collision that starts the watchdog).
  bool read(uint64_t t, uint64_t readUs)
  {
    if (watchdog) return false;
    if (t < readyAt) {
      ++collisions;
      watchdog = true;
      return false;
    }
    readyAt = t + readUs + scanUs;
    edgeOut = false;
    return true;
  }
  void check(uint64_t t)
  {
    if (!watchdog && t >= readyAt && t - readyAt > unreadLimitUs) {
      ++starvations;
      watchdog = true;
    }
  }
  void download(uint64_t t)
  {
    ++downloads;
    watchdog = false;
    readyAt = t + 190000 + scanUs + startUs;   // ~185 ms for the download, then the first frame (startUs: the controller's start)
    edgeOut = false;
  }
};

// The loop's rules: the current ones (TouchPace.h and PianoTouch::serve) or the previous ones (the attend images of 2026-11-11:
// the latch dropped after each idle sleep, 12 ms without an edge while busy, the idle period + 10 ms at rest and blind after three
// edge-less reads, 500 ms reads with 250 ms waits with the display off).
struct LoopRules {
  bool previous = false;
  bool noGrace = false;   // the current rules without TouchPace.h's StartGrace (before 2026-11-12's desk1 fix)
};
struct FeedRun {
  unsigned reads = 0, frames = 0, wakeups = 0, edgeReads = 0, levelReads = 0, blindReads = 0, downloads = 0;
  uint64_t firstSeenUs = 0, maxGapUs = 0;
  FeedModel m;
};
// loseEdges: every Nth edge never reaches the latch (0: none; 1: none at all, the latch broken). mutedUntil: the display off
// until then. A finger in [fingerOn, fingerOff). watchdogAt: the controller goes to 0xfd by itself then (0: never).
static FeedRun simulateFeed(LoopRules rules, uint64_t durationUs, uint64_t mutedUntilUs, uint64_t fingerOn, uint64_t fingerOff,
                            unsigned loseEdges = 0, uint64_t scanUs = 6500, uint64_t watchdogAt = 0, uint64_t startUs = 0)
{
  constexpr uint64_t kReadUs = 5100;
  FeedRun r;
  r.m.scanUs = scanUs;
  r.m.readyAt = scanUs;
  r.m.startUs = startUs;
  TouchPace pace = touchPace(kIdleReadMs, 0);
  WatchdogRecovery wd;
  StartGrace grace;
  uint64_t t = 0, lastRead = 0;
  bool latch = false, edgePending = false, rest = false, touching = false;
  unsigned edgeCount = 0, wdtRun = 0, prevBlind = 0;
  bool learning = true;
  while (t < durationUs) {
    ++r.wakeups;
    if (watchdogAt && t >= watchdogAt && !r.m.watchdog && r.m.downloads == 0) r.m.watchdog = true;
    if (!r.m.watchdog && !r.m.edgeOut && t >= r.m.readyAt) {   // the rising edge
      r.m.edgeOut = true;
      ++edgeCount;
      if (!loseEdges || (loseEdges > 1 && edgeCount % loseEdges)) latch = true;
    }
    r.m.check(t);
    if (latch) {
      latch = false;
      edgePending = true;
      grace.edge();
    }
    const bool muted = t < mutedUntilUs;
    const uint64_t since = t - lastRead;
    const bool level = r.m.pending(t);
    bool readNow;
    if (rules.previous) {
      if (muted) readNow = since >= 500000;
      else if (rest) readNow = edgePending || since >= (prevBlind >= 3 ? kIdleReadMs : kIdleReadMs + 10) * 1000ull;
      else readNow = edgePending || since >= 12000;
    } else {
      const bool levelPending = !edgePending && since >= kNoEdgeMinMs * 1000ull && level;
      if (!rules.noGrace && grace.holds(t / 1000)) readNow = edgePending || levelPending;   // after a start: the first frame's edge
      else if (muted) readNow = since >= kMutedReadMs * 1000ull;
      else if (rest) readNow = since >= pace.idleMs * 1000ull && (edgePending || levelPending || since >= pace.noEdgeMs(true) * 1000ull);
      else readNow = edgePending || levelPending || since >= pace.noEdgeMs(false) * 1000ull;
      if (readNow && !edgePending) (levelPending ? r.levelReads : r.blindReads)++;
    }
    if (readNow) {
      if (edgePending) ++r.edgeReads;
      if (rules.previous) prevBlind = edgePending ? 0 : prevBlind + 1;
      edgePending = false;
      if (lastRead && t - lastRead > r.maxGapUs) r.maxGapUs = t - lastRead;
      lastRead = t;
      ++r.reads;
      const bool frame = r.m.read(t, kReadUs);
      t += kReadUs;
      if (frame) {
        ++r.frames;
        wdtRun = 0;
        if (r.frames > 48) learning = false;   // the reference learned
        touching = t >= fingerOn && t < fingerOff;
        if (touching && !r.firstSeenUs) r.firstSeenUs = t;
      } else if (++wdtRun >= kWatchdogRun && !muted) {
        if (rules.previous ? r.m.downloads < 3 : wd.due(t / 1000)) {
          wd.downloaded(t / 1000);
          r.m.download(t);
          t += 190000;
          lastRead = t;   // as PianoTouch::serve: the edge-less wait starts again at the download's end
          edgePending = false;
          wdtRun = 0;
          learning = true;
          ++r.downloads;
          grace.started(t / 1000);
        }
      }
    }
    if (muted) {   // the display off
      t += rules.previous ? 250000 : std::max<uint64_t>(1000, kMutedReadMs * 1000ull - (t - lastRead));
      continue;
    }
    const bool busy = touching || learning || wdtRun;
    rest = pace.idle(t / 1000, busy);
    if (rest && !edgePending) {
      const uint64_t due = lastRead + pace.idleMs * 1000ull;
      if (t + (rules.previous && prevBlind < 3 ? 2000 : 0) < due) {
        t = due - (rules.previous && prevBlind < 3 ? 2000 : 0);
        if (rules.previous) {   // the latch dropped after the sleep
          if (!r.m.watchdog && !r.m.edgeOut && t >= r.m.readyAt) r.m.edgeOut = true;
          latch = false;
        }
        continue;
      }
    }
    t += 1000;
  }
  return r;
}

static void testPace()
{
  CHECK(TouchPace::clampIdle(0) == 0 && TouchPace::clampIdle(1) == kIdleReadMinMs && TouchPace::clampIdle(9999) == kIdleReadMaxMs);
  CHECK(kIdleReadMaxMs + kNoEdgeRestMs <= kMaxGapMs && kMutedReadMs <= kMaxGapMs && kNoEdgeMinMs * 1000 > kFrameCycleUs);
  CHECK(touchPace(50, 0).noEdgeMs(false) == kNoEdgeBusyMs && touchPace(50, 0).noEdgeMs(true) == 100 && touchPace(0, 0).noEdgeMs(false) == 200);
  CHECK(touchPace(50, 0, 1).noEdgeMs(false) == kNoEdgeMinMs && touchPace(50, 0, 999).noEdgeMs(false) == kNoEdgeOldMs);
  const LoopRules current, previous{true};
  // 1. The first runs, reproduced by the previous rules: the display off for 70 s at boot starves the controller (attend1); no
  //    edges at all while a finger is down makes the 12 ms reads collide with the scan (attend2: 0xfd at once, three downloads, given
  //    up). The current rules feed it in both.
  {
    const FeedRun prev = simulateFeed(previous, 80000000, 70000000, ~0ull, ~0ull);
    const FeedRun now = simulateFeed(current, 80000000, 70000000, ~0ull, ~0ull);
    std::printf("feed: display off 70 s at boot: previous rules %u watchdogs (max %llu ms unread), current %u (max gap %llu ms)\n",
                prev.m.starvations + prev.m.collisions, (unsigned long long)(prev.maxGapUs / 1000), now.m.starvations + now.m.collisions,
                (unsigned long long)(now.maxGapUs / 1000));
    CHECK(prev.m.starvations >= 1);
    CHECK(now.m.starvations == 0 && now.m.collisions == 0 && now.downloads == 0 && now.maxGapUs <= kMaxGapMs * 1000ull);
  }
  {
    const FeedRun prev = simulateFeed(previous, 30000000, 0, 10000000, 20000000, 1, 7500);   // no edges, a 12.6 ms cycle
    const FeedRun now = simulateFeed(current, 30000000, 0, 10000000, 20000000, 1, 7500);
    std::printf("feed: no edges, a finger for 10 s: previous rules %u collisions, %u downloads; current %u, %u (%u reads on the line, "
                "%u without)\n", prev.m.collisions, prev.downloads, now.m.collisions, now.downloads, now.levelReads, now.blindReads);
    CHECK(prev.m.collisions >= 1 && prev.downloads >= 1);
    CHECK(now.m.collisions == 0 && now.m.starvations == 0 && now.downloads == 0 && now.frames > 300);
  }
  // 2. The current rules in every case: edges always, every 3rd edge lost, none at all; cycles of 11.6 and 16 ms; with and without a
  //    finger; the display off at boot. Never a watchdog, at most kMaxGapMs between reads.
  for (unsigned lose : {0u, 3u, 1u})
    for (uint64_t scan : {6500ull, 11000ull})
      for (uint64_t muted : {0ull, 5000000ull}) {
        const FeedRun r = simulateFeed(current, 20000000, muted, 8000000, 12000000, lose, scan);
        CHECK(r.m.collisions == 0 && r.m.starvations == 0 && r.downloads == 0 && r.maxGapUs <= kMaxGapMs * 1000ull);
      }
  // 3. Under a finger every frame on its edge (~80 a second at an 11.6 ms cycle); at rest ~20; a touch at rest read within an idle
  //    period and a cycle.
  {
    const FeedRun busy = simulateFeed(current, 6000000, 0, 1000000, 7000000);
    CHECK(busy.frames >= 380 && busy.edgeReads >= busy.reads - 5);
    const FeedRun idle = simulateFeed(current, 12000000, 0, ~0ull, ~0ull);
    std::printf("feed: at rest %u reads and %u wake-ups in 12 s (learning included); a finger: %u frames in 5 s\n", idle.reads,
                idle.wakeups, busy.frames);
    CHECK(idle.reads >= 300 && idle.reads <= 420 && idle.wakeups <= 4 * idle.reads);   // ~50 learning, ~120 the hold, ~210 at rest
    uint64_t worst = 0;
    for (uint64_t phase = 0; phase < 60000; phase += 1777) {
      const uint64_t on = 8000000 + phase;
      const FeedRun r = simulateFeed(current, on + 300000, 0, on, on + 300000);
      CHECK(r.firstSeenUs && r.firstSeenUs - on <= (kIdleReadMs + 13 + 5) * 1000ull);
      if (r.firstSeenUs) worst = std::max(worst, r.firstSeenUs - on);
    }
    std::printf("feed: a touch at rest read at most %llu ms after it landed\n", (unsigned long long)(worst / 1000));
  }
  // 4. The watchdog by itself (the controller's own): the download at once, then every 10 s while it comes back, never given up.
  {
    WatchdogRecovery wd;
    CHECK(wd.due(1000));
    wd.downloaded(1000);
    CHECK(!wd.due(5000) && !wd.due(10999) && wd.due(11000));
    wd.downloaded(11000);
    CHECK(!wd.due(20000) && wd.due(21000) && wd.downloads == 2);
    const FeedRun r = simulateFeed(current, 10000000, 0, ~0ull, ~0ull, 0, 6500, 3000000);
    CHECK(r.downloads == 1 && !r.m.watchdog && r.frames > 100);   // it came back after the download
  }
  // 5. desk1 (2026-11-12): after a download the controller's first frame comes ~850 ms later. Without the start's grace the busy
  //    pace reads 50 ms after the download without an edge, inside the start: the watchdog again, download after download (every
  //    10 s, never back); with it the first read waits for the first edge and the controller comes back after one download.
  {
    StartGrace g;
    CHECK(!g.holds(0));
    g.started(1000);
    CHECK(g.holds(1500) && g.holds(2999) && !g.holds(3000) && g.expired == 1);
    g.started(5000);
    CHECK(g.holds(5100) && g.edge() && !g.edge() && !g.holds(5200) && g.edges == 1 && g.starts == 2);
    const LoopRules before{false, true};
    const FeedRun old = simulateFeed(before, 40000000, 0, ~0ull, ~0ull, 0, 6500, 3000000, 850000);
    const FeedRun now = simulateFeed(current, 40000000, 0, ~0ull, ~0ull, 0, 6500, 3000000, 850000);
    std::printf("feed: a start of 850 ms after each download: without the grace %u downloads, %u collisions (back: %s); with it %u, "
                "%u (back: %s)\n", old.downloads, old.m.collisions, old.m.watchdog ? "no" : "yes", now.downloads, now.m.collisions,
                now.m.watchdog ? "no" : "yes");
    CHECK(old.downloads >= 3 && old.m.collisions >= 2);
    CHECK(now.downloads == 1 && now.m.collisions == 0 && !now.m.watchdog && now.frames > 300);
    // The grace ends by itself: a line that never rises after the start is read without edges again 2 s later.
    const FeedRun dead = simulateFeed(current, 20000000, 0, ~0ull, ~0ull, 1, 6500, 3000000, 850000);
    CHECK(dead.reads > 50);
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
  testPoints();
  testTracker();
  testGestures();
  testGestureRotation();
  testBurstHold();
  testPower();
  testClock();
  testPace();
  if (failures) {
    std::printf("PianoTouch host tests: %d FAILED\n", failures);
    return 1;
  }
  std::printf("PASS: PianoTouch host tests (NT36532E model over whole transfers and by hand, %d real image(s), matrix, calibration, "
              "orientation, tip, HID, rotation, points, tracker, palms, gestures, gestures under rotation, burst hold, clocks, "
              "pace, power states, the probe)\n", argc - 1);
  return 0;
}
