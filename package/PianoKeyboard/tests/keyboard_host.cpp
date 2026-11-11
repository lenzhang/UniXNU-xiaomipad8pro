// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoKeyboard's portable parts: the WN8030 codec (Wn8030Piano.h: commands, the RAM image's header and data blocks,
// piano's packed frames, vendor messages), the pointer made of touchpad reports, BitI2c.h against a bit-level I2C target model
// (START/STOP, ACK/NACK, repeated START reads, clock stretching), CmdDb.h on a small database, and HidDescriptors.h (parsed item by
// item: each report is as long as the driver sends it).
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. keyboard_host.cpp -o /tmp/keyboard_host && /tmp/keyboard_host [--no-firmware] [MCU_Upgrade.bin]
#include "Wn8030Piano.h"
#include "BitI2c.h"
#include "CmdDb.h"
#include "HidDescriptors.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

using namespace pianokeyboard;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ------------------------------------------------------------------------------------------------------------- the I2C target
struct Target {
  uint8_t address = kAddress;
  bool sdaLow = false, sclLow = false;      // what the target drives
  bool mSda = true, mScl = true;            // what the master leaves (true: released)
  bool lastSda = true, lastScl = true;
  enum { idle, addr, ackAddr, wr, ackWr, rd, rdAck, ignore } st = idle;
  int bits = 0;
  uint8_t shift = 0, byte = 0;
  bool reading = false, masterAck = false;
  std::vector<uint8_t> cur;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> readData;
  size_t readPos = 0;
  int stretch = 0, holding = 0, starts = 0, stops = 0;
  bool sda() const { return mSda && !sdaLow; }
  bool scl() const { return mScl && !sclLow; }
  void finish() { if (!cur.empty()) writes.push_back(cur); cur.clear(); }
  void load() { byte = readPos < readData.size() ? readData[readPos++] : 0xff; sdaLow = !(byte & 0x80); }
  void rising()
  {
    if (st == addr || st == wr) { shift = uint8_t(shift << 1 | (sda() ? 1 : 0)); ++bits; }
    else if (st == rdAck) masterAck = !sda();
  }
  void falling()
  {
    switch (st) {
    case addr:
      if (bits == 8) {
        if ((shift >> 1) == address) { st = ackAddr; reading = shift & 1; sdaLow = true; }
        else st = ignore;
      }
      break;
    case ackAddr:
      sdaLow = false;
      if (stretch) { sclLow = true; holding = stretch; }
      if (reading) { st = rd; bits = 0; load(); }
      else { st = wr; bits = 0; shift = 0; }
      break;
    case wr:
      if (bits == 8) { cur.push_back(shift); st = ackWr; sdaLow = true; }
      break;
    case ackWr: sdaLow = false; st = wr; bits = 0; shift = 0; break;
    case rd:
      if (++bits < 8) sdaLow = !((byte >> (7 - bits)) & 1);
      else { sdaLow = false; st = rdAck; }
      break;
    case rdAck:
      if (masterAck) { st = rd; bits = 0; load(); }
      else { st = ignore; sdaLow = false; }
      break;
    default: break;
    }
  }
  void update()
  {
    const bool s = sda(), c = scl();
    if (c && lastScl && lastSda && !s) { finish(); st = addr; bits = 0; shift = 0; ++starts; }   // START
    else if (c && lastScl && !lastSda && s) { finish(); st = idle; ++stops; }               // STOP
    else if (c && !lastScl) rising();
    else if (!c && lastScl) falling();
    lastSda = sda();
    lastScl = scl();
  }
};
static Target gT;
static void tSda(void *, bool release) { gT.mSda = release; gT.update(); }
static void tScl(void *, bool release) { gT.mScl = release; gT.update(); }
static bool tReadSda(void *) { return gT.sda(); }
static bool tReadScl(void *) { return gT.scl(); }
static unsigned gDelays;
static void tDelay(void *, unsigned)
{
  ++gDelays;
  if (gT.holding && !--gT.holding) { gT.sclLow = false; gT.update(); }
}

static void testI2c()
{
  const LineIo io = {nullptr, tSda, tScl, tReadSda, tReadScl, tDelay};
  BitI2c bus;
  bus.configure(io, 2, 20000);
  // Boot ROM id: {0x5c, 0x04}, then register 0x5c read back.
  const uint8_t cmd[2] = {kRegBoot, kRomId};
  CHECK(bus.write(kAddress, cmd, 2) == I2c::ok);
  CHECK(gT.writes.size() == 1 && gT.writes[0] == std::vector<uint8_t>(cmd, cmd + 2));
  gT.readData = {kBootId};
  gT.readPos = 0;
  uint8_t v = 0;
  const uint8_t reg = kRegBoot;
  CHECK(bus.writeRead(kAddress, &reg, 1, &v, 1) == I2c::ok);
  CHECK(v == kBootId);
  CHECK(gT.writes.size() == 2 && gT.writes[1] == std::vector<uint8_t>{kRegBoot});
  // A 68-byte frame, with the target stretching the clock after the address.
  gT.stretch = 40;
  std::vector<uint8_t> frame(kFrameBytes);
  for (unsigned i = 0; i < kFrameBytes; ++i) frame[i] = uint8_t(i * 37 + 0x57);
  gT.readData = frame;
  gT.readPos = 0;
  uint8_t got[kFrameBytes] = {};
  const uint8_t runtime = kRegRuntime;
  CHECK(bus.writeRead(kAddress, &runtime, 1, got, kFrameBytes) == I2c::ok);
  CHECK(std::memcmp(got, frame.data(), kFrameBytes) == 0);
  gT.stretch = 0;
  // A data block (513 bytes), and a command.
  std::vector<uint8_t> block(kDataWire);
  for (unsigned i = 0; i < kDataWire; ++i) block[i] = uint8_t(i ^ 0xa5);
  CHECK(bus.write(kAddress, block.data(), kDataWire) == I2c::ok);
  CHECK(gT.writes.back() == block);
  uint8_t wire[kCommandWire];
  command(wire, kCmdTouchpad, 1);
  CHECK(bus.write(kAddress, wire, kCommandWire) == I2c::ok);
  CHECK(gT.writes.back() == std::vector<uint8_t>(wire, wire + kCommandWire));
  // Nobody at 0x4d; a target that never releases SCL.
  CHECK(bus.write(0x4d, cmd, 2) == I2c::nackAddress);
  gT.stretch = 1000000;
  CHECK(bus.write(kAddress, cmd, 2) == I2c::stretchTimeout);
  gT.holding = 0;
  gT.sclLow = false;
  gT.stretch = 0;
  gT.update();
  CHECK(bus.recover());
  CHECK(bus.write(kAddress, cmd, 2) == I2c::ok);
  CHECK(gT.starts >= 7 && gT.stops >= 7);
}

// --------------------------------------------------------------------------------------------------------------------- the codec
static void testCommands()
{
  uint8_t w[kCommandWire];
  command(w, kCmdIndicator, kLedCapsOn);
  const uint8_t head[11] = {0x4c, 0x32, 0x00, 0x4e, 0x31, 0x80, 0x38, 0x2e, 0x01, 0xfd,
                            uint8_t(0x4e + 0x31 + 0x80 + 0x38 + 0x2e + 0x01 + 0xfd)};
  CHECK(std::memcmp(w, head, sizeof head) == 0);
  bool rest = true;
  for (unsigned i = sizeof head; i < kCommandWire; ++i) rest = rest && !w[i];
  CHECK(rest);
}

static void testImage(const char *path, bool noFirmware)
{
  std::vector<uint8_t> image;
  if (FILE *f = noFirmware ? nullptr : std::fopen(path, "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f))) image.insert(image.end(), buf, buf + n);
    std::fclose(f);
  }
  bool real = !image.empty();
  if (!real) {   // a synthetic image: header, 1210 bytes of code
    image.assign(kImageCode + 1210 + 2, 0);
    std::memcpy(&image[kImageHeader], "NANO IC", 7);
    uint32_t sum = 0;
    for (unsigned i = 0; i < 1210 + 2; ++i) sum += image[kImageCode + i] = uint8_t(i * 7);
    putLe32(&image[kImageHeader + 48], 1212);
    putLe32(&image[kImageHeader + 52], sum);
  }
  uint32_t code = 0;
  CHECK(imageCheck(image.data(), uint32_t(image.size()), &code) == 0);
  if (real) CHECK(code == 50668);
  uint8_t h[kHeaderWire];
  headerCommand(h, image.data(), code);
  CHECK(h[0] == kRegBoot && h[1] == kRomHeader && std::memcmp(h + 2, "NANO IC", 7) == 0);
  CHECK(h[1 + 64] == sum8(h + 2, 63) && le32(h + 1 + 18) == code);
  uint32_t words = 0;
  for (uint32_t i = 0; i < code; i += 4) words += le32(&image[kImageCode + i]);
  CHECK(le32(h + 1 + 53) == words);
  unsigned blocks = 0, total = 0;
  uint8_t d[kDataWire];
  for (uint32_t offset = 0;; ++blocks) {
    const unsigned n = dataCommand(d, image.data(), code, offset);
    if (!n) break;
    CHECK(d[0] == kRegBoot && d[1] == kRomData && (uint32_t(d[2]) << 24 | d[3] << 16 | d[4] << 8 | d[5]) == offset);
    CHECK(std::memcmp(d + 10, &image[kImageCode + offset], n) == 0);
    offset += n;
    total += n;
  }
  CHECK(total == code && blocks == (code + kDataChunk - 1) / kDataChunk);
  if (real) CHECK(blocks == 101);
  image[kImageCode + 5] ^= 1;
  CHECK(imageCheck(image.data(), uint32_t(image.size()), &code) == -4);
  image[kImageHeader] = 'X';
  CHECK(imageCheck(image.data(), uint32_t(image.size()), &code) == -2);
  std::printf("keyboard_host: image %s (%u code bytes, %u blocks)\n", real ? path : "synthetic", total, blocks);
}

static void testFrames()
{
  uint8_t f[kFrameBytes] = {0x57, 0x00, 0x39};
  const uint8_t key[9] = {kIdKeyboard, 0x02, 0, 0x04, 0, 0, 0, 0, 0}, consumer[5] = {kIdConsumer, 0xe9, 0, 0, 0};
  std::memcpy(f + 3, key, 9);
  std::memcpy(f + 12, consumer, 5);
  Report r[24];
  unsigned n = 0;
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 2);
  CHECK(r[0].id == kIdKeyboard && r[0].length == 9 && r[0].bytes == f + 3);
  CHECK(r[1].id == kIdConsumer && r[1].length == 5 && le16(r[1].bytes + 1) == 0xe9);
  f[0] = 0xd7;   // the top bit is ignored
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 2);
  f[2] = 0;
  CHECK(unpackFrame(f, r, 24, &n) == Frame::empty);
  f[2] = 0x40;
  CHECK(unpackFrame(f, r, 24, &n) == Frame::badType);
  f[2] = 0x4a;
  f[0] = 0x56;
  CHECK(unpackFrame(f, r, 24, &n) == Frame::badMagic);
  f[0] = 0x00;
  CHECK(unpackFrame(f, r, 24, &n) == Frame::empty);
  f[0] = 0x57;
  f[17] = 0x77;
  CHECK(unpackFrame(f, r, 24, &n) == Frame::unknownId && n == 2);
  // Two touchpad reports fill 3 + 54 bytes; a third would run past the end.
  uint8_t t[kFrameBytes] = {0x57, 0, 0x6c};
  t[3] = kIdTouchpad;
  t[30] = kIdTouchpad;
  CHECK(unpackFrame(t, r, 24, &n) == Frame::reports && n == 2 && r[1].bytes == t + 30);
  t[57] = kIdTouchpad;
  CHECK(unpackFrame(t, r, 24, &n) == Frame::overrun && n == 2);
  // Vendor: an attach message; a long one to the end of the frame.
  uint8_t v[kFrameBytes] = {0x57, 0, 0x5b, 0x22, 0x10, 0x38, 0x80, 0xa2, 0, 0, 0, 0, 0x01};
  CHECK(unpackFrame(v, r, 24, &n) == Frame::reports && n == 1 && r[0].length == 16);
  int attached = -1;
  CHECK(vendorKind(r[0], &attached) == Vendor::attach && attached == 1);
  v[12] = 0x02;
  CHECK(unpackFrame(v, r, 24, &n) == Frame::reports && vendorKind(r[0], &attached) == Vendor::attach && attached == 0);
  v[19] = 0x24;
  v[21] = 0x38;
  v[22] = 0x80;
  v[23] = 0x24;
  CHECK(unpackFrame(v, r, 24, &n) == Frame::reports && n == 2 && r[1].length == kFrameBytes - 19);
  CHECK(vendorKind(r[1], &attached) == Vendor::authRequest);
  CHECK(vendorKind(r[0], &attached) == Vendor::attach);
  Report key5 = {kIdKeyboard, 9, key};
  CHECK(vendorKind(key5, &attached) == Vendor::none);
}

// A touchpad report: buttons and up to three {flags, id, x, y}.
struct Finger { uint8_t flags, id; uint16_t x, y; };
static void touchpad(uint8_t out[kTouchpadBytes], uint8_t buttons, std::initializer_list<Finger> fingers)
{
  std::memset(out, 0, kTouchpadBytes);
  out[0] = kIdTouchpad;
  out[1] = buttons;
  unsigned i = 0;
  for (const Finger &f : fingers) {
    uint8_t *p = out + 2 + 8 * i++;
    p[0] = f.flags;
    p[1] = f.id;
    putLe16(p + 4, f.x);
    putLe16(p + 6, f.y);
  }
  out[26] = uint8_t(i);
}
static int16_t px(const uint8_t *o) { return int16_t(le16(o + 1)); }
static int16_t py(const uint8_t *o) { return int16_t(le16(o + 3)); }

static void testPointer()
{
  Pointer p;
  uint8_t tp[kTouchpadBytes], out[2][kPointerBytes];
  constexpr uint8_t kTouch = 1 | 2 | 4;
  touchpad(tp, 0, {{kTouch, 3, 1000, 1000}});
  CHECK(p.touch(tp, 0, out) == 0);                     // a finger lands: no movement
  touchpad(tp, 0, {{kTouch, 3, 1101, 980}});
  CHECK(p.touch(tp, 10, out) == 1 && px(out[0]) == 50 && py(out[0]) == -10 && out[0][0] == 0);
  touchpad(tp, 0, {{kTouch, 3, 1102, 980}});           // the half count left over moves now
  CHECK(p.touch(tp, 20, out) == 1 && px(out[0]) == 1 && py(out[0]) == 0);
  touchpad(tp, 0, {{kTouch, 3, 1102, 980}, {kTouch, 4, 2000, 1000}});   // a second finger: no jump
  CHECK(p.touch(tp, 30, out) == 0);
  touchpad(tp, 0, {{kTouch, 3, 1102, 880}, {kTouch, 4, 2000, 900}});    // both up 100 counts: two wheel steps up
  CHECK(p.touch(tp, 40, out) == 1 && out[0][5] == 2 && px(out[0]) == 0);
  touchpad(tp, 1, {{kTouch, 3, 1102, 880}, {kTouch, 4, 2000, 900}});    // a press with two fingers: the right button
  CHECK(p.touch(tp, 50, out) == 1 && out[0][0] == 2);
  touchpad(tp, 0, {});
  CHECK(p.touch(tp, 600, out) == 1 && out[0][0] == 0);                  // released; a long touch is no tap
  // A tap: down and up within 180 ms, hardly moving.
  touchpad(tp, 0, {{kTouch, 5, 500, 500}});
  CHECK(p.touch(tp, 1000, out) == 0);
  touchpad(tp, 0, {{kTouch, 5, 510, 505}});
  p.touch(tp, 1040, out);
  touchpad(tp, 0, {});
  CHECK(p.touch(tp, 1090, out) == 2 && out[0][0] == 1 && out[1][0] == 0 && p.taps() == 1);
  // A palm (touching, no confidence) is ignored.
  touchpad(tp, 0, {{1 | 2, 6, 500, 500}});
  CHECK(p.touch(tp, 2000, out) == 0);
  touchpad(tp, 0, {{1 | 2, 6, 900, 500}});
  CHECK(p.touch(tp, 2010, out) == 0);
  // The MCU's own mouse report.
  const uint8_t m[kMouseBytes] = {kIdMouse, 0x01, 0xfe, 0xff, 0x03, 0x00, 0x00, 0x01};
  uint8_t mo[kPointerBytes];
  Pointer::mouse(m, mo);
  CHECK(mo[0] == 1 && px(mo) == -2 && py(mo) == 3 && mo[5] == 127);
}

// ------------------------------------------------------------------------------------------------------------------- cmd-db
static void testCmdDb()
{
  std::vector<uint8_t> db(1024, 0);
  putLe32(&db[4], kCmdDbMagic);
  // Slave 0 (id 3, ARC) one entry; slave 1 (id 4, VRM) two entries.
  auto slave = [&](unsigned s, uint16_t id, uint16_t heads, uint16_t count) {
    putLe16(&db[8 + 16 * s], id); putLe16(&db[8 + 16 * s + 2], heads); putLe16(&db[8 + 16 * s + 6], count);
  };
  auto entry = [&](size_t at, const char *name, uint32_t address) {
    std::memcpy(&db[at], name, std::strlen(name)); putLe32(&db[at + 16], address);
  };
  slave(0, 3, 0, 1);
  slave(1, 4, 24, 2);
  entry(144, "lcx.lvl", 0x30000);
  entry(144 + 24, "ldob11", 0x40100);
  entry(144 + 48, "ldob14", 0x40400);
  uint32_t a = 0;
  CHECK(cmdDbFind(db.data(), db.size(), "ldob11", &a) == 0 && a == 0x40100);
  CHECK(cmdDbFind(db.data(), db.size(), "ldob14", &a) == 0 && a == 0x40400);
  CHECK(cmdDbFind(db.data(), db.size(), "ldob1", &a) == -2);
  db[4] = 0;
  CHECK(cmdDbFind(db.data(), db.size(), "ldob11", &a) == -1);
  CHECK(rscLayout(0x30000).cmdStride == 24 && rscLayout(0x20000).cmdStride == 20);
}

// ----------------------------------------------------------------------------------------------------------- the descriptors
// Input/output bits per report ID (0: no ID), from the global Report Size/Count/ID items.
static void parse(const uint8_t *d, unsigned n, std::map<int, unsigned> &in, std::map<int, unsigned> &out, int *depth)
{
  unsigned size = 0, count = 0;
  int id = 0;
  *depth = 0;
  for (unsigned i = 0; i < n;) {
    const uint8_t item = d[i];
    const unsigned len = (item & 3) == 3 ? 4 : item & 3;
    uint32_t v = 0;
    for (unsigned k = 0; k < len; ++k) v |= uint32_t(d[i + 1 + k]) << (8 * k);
    switch (item & 0xfc) {
    case 0x74: size = v; break;
    case 0x94: count = v; break;
    case 0x84: id = int(v); break;
    case 0x80: in[id] += size * count; break;
    case 0x90: out[id] += size * count; break;
    case 0xa0: ++*depth; break;
    case 0xc0: --*depth; break;
    default: break;
    }
    i += 1 + len;
  }
}
static void testDescriptors()
{
  std::map<int, unsigned> in, out;
  int depth = 0;
  parse(kKeyboardDescriptor, sizeof kKeyboardDescriptor, in, out, &depth);
  CHECK(depth == 0 && in.size() == 2 && out.size() == 1);
  CHECK(1 + in[kIdKeyboard] / 8 == kKeyboardBytes && 1 + in[kIdConsumer] / 8 == kConsumerBytes && out[kIdKeyboard] == 8);
  in.clear();
  out.clear();
  parse(kPointerDescriptor, sizeof kPointerDescriptor, in, out, &depth);
  CHECK(depth == 0 && in.size() == 1 && out.empty() && in[0] / 8 == kPointerBytes);
}

int main(int argc, char **argv)
{
  bool noFirmware = false;
  const char *firmware = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--no-firmware")) noFirmware = true;
    else firmware = argv[i];
  }
  testCommands();
  testImage(firmware ? firmware : "", noFirmware || !firmware);
  testFrames();
  testPointer();
  testI2c();
  testCmdDb();
  testDescriptors();
  if (failures) {
    std::printf("keyboard_host: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("keyboard_host: all passed (%u I2C waits)\n", gDelays);
  return 0;
}
