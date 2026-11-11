// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of PianoKeyboard's portable parts: the WN8030 codec (Wn8030Piano.h: commands, the RAM image's header and data blocks,
// piano's packed frames, vendor messages; the frames captured on piano 2026-11-11 and key, consumer, touchpad, attach and
// authentication frames of the same shape), the keyboard filter (rollover, the opt-in Fn row), the pointer made of touchpad reports
// (swipes, scrolling, taps, the contact count, the press button, the axes' scale), BitI2c.h against a bit-level I2C target model
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
  // The Linux driver's authentication answers (channel 0x4f).
  authInit(w);
  const uint8_t init[15] = {0x4c, 0x32, 0x00, 0x4f, 0x31, 0x80, 0x38, 0x31, 0x06, 'M', 'I', 'A', 'U', 'T', 'H'};
  CHECK(std::memcmp(w, init, sizeof init) == 0 && w[15] == sum8(w + 3, 12) && !w[16]);
  authS3t1(w);
  const uint8_t s3t1[29] = {0x4c, 0x32, 0x00, 0x4f, 0x31, 0x80, 0x38, 0x32, 0x14, 0x00, 0x00, 0x00, 0x02, 0x80, 0x3d,
                            0x84, 0x36, 0x29, 0x82, 0xde, 0x6a, 0x0e, 0x68, 0x1d, 0x3d, 0x6b, 0x32, 0xa5, 0xa6};
  CHECK(std::memcmp(w, s3t1, sizeof s3t1) == 0 && w[29] == sum8(w + 3, 26) && !w[30]);
  CHECK(!message(w, kChannelControl, 0x99, init, 58));
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
  touchpad(tp, 0, {{kTouch, 3, 1101, 980}});           // X 101 counts: 50; Y -20 counts = -13.3 X counts: -6
  CHECK(p.touch(tp, 10, out) == 1 && px(out[0]) == 50 && py(out[0]) == -6 && out[0][0] == 0);
  touchpad(tp, 0, {{kTouch, 3, 1102, 980}});           // the half count left over moves now
  CHECK(p.touch(tp, 20, out) == 1 && px(out[0]) == 1 && py(out[0]) == 0);
  touchpad(tp, 0, {{kTouch, 3, 1102, 980}, {kTouch, 4, 2000, 1000}});   // a second finger: no jump
  CHECK(p.touch(tp, 30, out) == 0);
  touchpad(tp, 0, {{kTouch, 3, 1102, 880}, {kTouch, 4, 2000, 900}});    // both up 100 Y counts (~2 mm): one wheel step up
  CHECK(p.touch(tp, 40, out) == 1 && out[0][5] == 1 && px(out[0]) == 0);
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

// ------------------------------------------------------------------------------------------------ frames as piano sends them
// Hex of the 2026-11-11 capture (logs/usb-console/20261111-084701-port0.txt): whole frames, or one report of a frame.
static std::vector<uint8_t> unhex(const char *s)
{
  std::vector<uint8_t> v;
  for (unsigned hi = 0, half = 0; *s; ++s) {
    const char c = *s;
    const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    if (d < 0) continue;
    if (half) { v.push_back(uint8_t(hi << 4 | unsigned(d))); half = 0; }
    else { hi = unsigned(d); half = 1; }
  }
  return v;
}
// A frame as the cover packs it: {0x57, sequence, type 0x39, reports..., zeros, the trailer sum}.
static void pack(uint8_t f[kFrameBytes], uint8_t seq, std::initializer_list<std::vector<uint8_t>> reports)
{
  std::memset(f, 0, kFrameBytes);
  f[0] = 0x57;
  f[1] = seq;
  f[2] = 0x39;
  unsigned at = 3;
  for (const std::vector<uint8_t> &r : reports) {
    std::memcpy(f + at, r.data(), r.size() < kFrameBytes - at ? r.size() : kFrameBytes - at);
    at += unsigned(r.size());
  }
  f[kFrameBytes - 1] = sum8(f + 2, kFrameBytes - 3);
}
// A vendor message {id, 0x31, 0x38, 0x80, kind, length, payload, its byte sum}, padded to the ID's size.
static std::vector<uint8_t> vendor(uint8_t id, uint8_t kind, std::initializer_list<uint8_t> payload, unsigned size)
{
  std::vector<uint8_t> v = {id, 0x31, 0x38, 0x80, kind, uint8_t(payload.size())};
  v.insert(v.end(), payload);
  v.push_back(sum8(v.data(), unsigned(v.size())));
  v.resize(size, 0);
  return v;
}

static void testCaptured()
{
  // The first three frames, whole: vendor 0x23 kind 0xa4 (meaning unknown), each frame's trailer the sum of bytes 2..66.
  const char *frames[3] = {"57 01 39 23 31 38 80 a4 05 01 00 00 ff ff b4", "57 02 39 23 31 38 80 a4 05 01 00 ff 01 ff b5",
                           "57 03 39 23 31 38 80 a4 05 01 00 ff 01 01 b7"};
  const uint8_t trailers[3] = {0xa1, 0xa3, 0xa7};
  Report r[24];
  unsigned n = 0;
  int attached = -1;
  for (unsigned i = 0; i < 3; ++i) {
    uint8_t f[kFrameBytes] = {};
    const std::vector<uint8_t> head = unhex(frames[i]);
    std::memcpy(f, head.data(), head.size());
    f[67] = trailers[i];
    CHECK(frameSumOk(f));
    CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 1 && r[0].id == 0x23 && r[0].length == 32);
    CHECK(vendorKind(r[0], &attached) == Vendor::other && attached == -1 && !authPayload(r[0]));
    CHECK(sum8(r[0].bytes, 11) == r[0].bytes[11]);   // the message's own sum: bytes 0..5+length
    f[20] ^= 0x10;
    CHECK(!frameSumOk(f) && unpackFrame(f, r, 24, &n) == Frame::reports);   // a bad sum is only a diagnostic
  }
  // The vendor messages after them, each in a frame of its own as the cover sent them.
  struct Seen { const char *hex; Vendor kind; int attached; };
  const Seen seen[] = {
    {"22 31 38 80 28 01 01 35 00 00 00 00 00 00 00 00", Vendor::other, -1},
    {"23 31 38 80 a2 0d 01 00 ff 23 f9 01 02 10 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00", Vendor::attach, 1},
    {"22 31 38 80 24 01 01 31 00 00 00 00 00 00 00 00", Vendor::authRequest, -1},
    {"23 30 38 80 01 0e 19 01 01 0b 46 94 12 10 06 00 00 00 00 01 43 00 00 00 00 00 00 00 00 00 00 00", Vendor::other, -1},
    {"22 31 38 80 f0 02 25 00 22 00 00 00 00 00 00 00", Vendor::ack, -1},
    {"22 31 38 80 f0 02 2e 00 2b 00 00 00 00 00 00 00", Vendor::ack, -1},
    {"22 31 38 80 25 01 01 32 00 00 00 00 00 00 00 00", Vendor::state, -1},
    {"22 31 38 80 2e 01 fc 36 00 00 00 00 00 00 00 00", Vendor::state, -1},
    {"22 31 38 80 21 01 01 2e 00 00 00 00 00 00 00 00", Vendor::state, -1},
    {"22 31 38 80 64 06 04 00 c4 ff 04 f5 35 00 00 00", Vendor::accel, -1},
    {"22 31 38 80 24 01 64 94 00 00 00 00 00 00 00 00", Vendor::authRequest, -1},
  };
  uint8_t seq = 4;
  for (const Seen &s : seen) {
    uint8_t f[kFrameBytes];
    pack(f, seq++, {unhex(s.hex)});
    CHECK(frameSumOk(f));
    CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 1);
    CHECK(vendorKind(r[0], &attached) == s.kind && attached == s.attached);
    const unsigned length = r[0].bytes[5];
    if (r[0].bytes[1] == 0x31 && 6 + length < 16) CHECK(sum8(r[0].bytes, 6 + length) == r[0].bytes[6 + length]);   // 16 logged
  }
  // The ack names the command and its status; the state message the command's value.
  uint8_t f[kFrameBytes];
  pack(f, 20, {unhex(seen[4].hex), unhex(seen[8].hex)});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 2 && r[0].bytes[6] == kCmdKeyboardPower && r[0].bytes[7] == 0);
  CHECK(r[1].bytes[4] == kCmdTouchpad && r[1].bytes[6] == 1);
  // Under 12 bytes or off the 0x38 0x80 route: "other", as Linux ignores it.
  Report shortOne = {0x22, 11, r[0].bytes};
  CHECK(vendorKind(shortOne, &attached) == Vendor::other);
}

// Attach and detach: the attach bit (byte 9 bit 0, as v1.1.0 reads it), and the commands that follow.
static void testAttach()
{
  Report r[24];
  unsigned n = 0;
  int attached = -1;
  const uint8_t states[] = {0x23, 0x01, 0x21, 0x22, 0x02, 0x00};
  const int want[] = {1, 1, 1, 0, 0, 0};
  for (unsigned i = 0; i < sizeof states; ++i) {
    uint8_t f[kFrameBytes];
    pack(f, uint8_t(i), {vendor(0x23, 0xa2, {0x01, 0x00, 0xff, states[i], 0xf9, 0x01, 0x02, 0x10}, 32)});
    CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && vendorKind(r[0], &attached) == Vendor::attach && attached == want[i]);
  }
  Command c[5];
  CHECK(attachCommands(false, 1, 50, c) == 5);
  CHECK(c[0].cmd == kCmdKeyboardPower && c[0].value == 1 && c[1].cmd == kCmdIndicator && c[1].value == kLedCapsOff);
  CHECK(c[2].cmd == kCmdTouchpad && c[2].value == 1 && c[3].cmd == kCmdIndicator && c[3].value == kLedMicOff);
  CHECK(c[4].cmd == kCmdBacklight && c[4].value == 50);
  CHECK(attachCommands(true, 2, 101, c) == 3 && c[1].value == kLedCapsOn && c[2].value == kLedMicOff);   // no touchpad, no backlight
  CHECK(attachCommands(false, 0, 0, c) == 5 && c[2].value == 0 && c[4].value == 0);
  // The touchpad-on command on the wire, as the cover acknowledged it on 2026-11-11.
  uint8_t w[kCommandWire];
  command(w, kCmdTouchpad, 1);
  const std::vector<uint8_t> touchpadOn = unhex("4c 32 00 4e 31 80 38 21 01 01");
  CHECK(std::memcmp(w, touchpadOn.data(), touchpadOn.size()) == 0 && w[10] == sum8(w + 3, 7));
}

// The authentication messages: what is answered, what is too short, what carries material that is never logged.
static void testAuth()
{
  Report r[24];
  unsigned n = 0;
  uint8_t wire[kCommandWire];
  // A request (0x24): the init {0x31, "MIAUTH"} on channel 0x4f.
  uint8_t f[kFrameBytes];
  pack(f, 1, {unhex("22 31 38 80 24 01 64 94 00 00 00 00 00 00 00 00")});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && authReply(r[0], wire) == AuthStep::init);
  CHECK(wire[3] == kChannelAuth && wire[7] == kAuthInit && std::memcmp(wire + 9, "MIAUTH", 6) == 0 && !authPayload(r[0]));
  // The cover's init reply (0x31, UID at +8) in a 32-byte report: the offline s3t1. In a 16-byte one: too short.
  std::vector<uint8_t> uid = vendor(0x23, 0x31, {0, 0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
                                                 0xee, 0xf0, 0x01}, 32);
  pack(f, 2, {uid});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && authPayload(r[0]) && authReply(r[0], wire) == AuthStep::s3t1);
  CHECK(wire[7] == kAuthS3t1 && wire[8] == 20 && wire[12] == 0x02 && wire[13] == 0x80);
  pack(f, 3, {vendor(0x22, 0x31, {0, 0, 0x11, 0x22}, 16)});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && authReply(r[0], wire) == AuthStep::tooShort);
  // The challenge (0x32, 16 bytes at +22): only a 0x24/0x26 report (to the end of the frame) is long enough; never answered.
  std::vector<uint8_t> challenge = vendor(0x24, 0x32, {}, 7);
  challenge.resize(40, 0x5a);
  pack(f, 4, {challenge});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 1 && r[0].length == kFrameBytes - 3);
  CHECK(authPayload(r[0]) && authReply(r[0], wire) == AuthStep::challenge);
  pack(f, 5, {vendor(0x23, 0x32, {}, 32)});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && authReply(r[0], wire) == AuthStep::tooShort);
  // Nothing else is answered.
  pack(f, 6, {unhex("22 31 38 80 25 01 01 32 00 00 00 00 00 00 00 00")});
  CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && authReply(r[0], wire) == AuthStep::none);
}

// Keyboard reports in frames shaped like the captured ones, through the filter that makes what macOS gets.
static std::vector<uint8_t> keys(uint8_t mods, std::initializer_list<uint8_t> usages)
{
  std::vector<uint8_t> k = {kIdKeyboard, mods, 0};
  k.insert(k.end(), usages);
  k.resize(kKeyboardBytes, 0);
  return k;
}
static bool sentIs(const uint8_t *out, uint8_t mods, std::initializer_list<uint8_t> usages)
{
  const std::vector<uint8_t> want = keys(mods, usages);
  return std::memcmp(out, want.data(), kKeyboardBytes) == 0;
}
static void testKeys()
{
  Report r[24];
  unsigned n = 0;
  uint8_t f[kFrameBytes], out[kKeyboardBytes];
  Keys k;
  // 'a' down, Shift+'a', both up, packed with a consumer key (volume up) and its release in one frame.
  pack(f, 1, {keys(0, {0x04}), keys(0x02, {0x04}), keys(0, {}), {kIdConsumer, 0xe9, 0x00, 0x00, 0x00}, {kIdConsumer, 0, 0, 0, 0}});
  CHECK(frameSumOk(f) && unpackFrame(f, r, 24, &n) == Frame::reports && n == 5);
  k.filter(r[0].bytes, out);
  CHECK(sentIs(out, 0, {0x04}));
  k.filter(r[1].bytes, out);
  CHECK(sentIs(out, 0x02, {0x04}));
  k.filter(r[2].bytes, out);
  CHECK(sentIs(out, 0, {}));
  CHECK(r[3].length == 5 && le16(r[3].bytes + 1) == 0xe9 && r[4].length == 5 && le16(r[4].bytes + 1) == 0);
  // Every modifier bit passes; the mic key (0x6f, F20) and the keys up to 0xa4 pass as they are.
  for (unsigned m = 0; m < 8; ++m) {
    const std::vector<uint8_t> in = keys(uint8_t(1u << m), {0x6f, 0xa4});
    k.filter(in.data(), out);
    CHECK(std::memcmp(out, in.data(), kKeyboardBytes) == 0);
  }
  // Six keys, then ErrorRollOver: the six stay down, the new modifiers count; then a real report again.
  const std::vector<uint8_t> six = keys(0, {0x04, 0x05, 0x06, 0x07, 0x08, 0x09});
  k.filter(six.data(), out);
  const std::vector<uint8_t> roll = keys(0x01, {0x01, 0x01, 0x01, 0x01, 0x01, 0x01});
  k.filter(roll.data(), out);
  CHECK(sentIs(out, 0x01, {0x04, 0x05, 0x06, 0x07, 0x08, 0x09}) && k.rollovers() == 1);
  const std::vector<uint8_t> five = keys(0, {0x04, 0x05, 0x06, 0x07, 0x08});
  k.filter(five.data(), out);
  CHECK(sentIs(out, 0, {0x04, 0x05, 0x06, 0x07, 0x08}));
  // Without fnRow (the default), Right Ctrl + 1 is Ctrl+1.
  const std::vector<uint8_t> ctrl1 = keys(kModRightCtrl, {0x1e});
  k.filter(ctrl1.data(), out);
  CHECK(sentIs(out, kModRightCtrl, {0x1e}));
  // With it: Right Ctrl + 1 is F1 without Ctrl; Ctrl let go first, F1 stays until 1 is up; = and 0 are F12 and F10.
  Keys fn;
  fn.fnRow = true;
  fn.filter(ctrl1.data(), out);
  CHECK(sentIs(out, 0, {0x3a}) && fn.fnHeld() == 1);
  const std::vector<uint8_t> one = keys(0, {0x1e});
  fn.filter(one.data(), out);
  CHECK(sentIs(out, 0, {0x3a}));
  const std::vector<uint8_t> none = keys(0, {});
  fn.filter(none.data(), out);
  CHECK(sentIs(out, 0, {}) && !fn.fnHeld());
  fn.filter(one.data(), out);   // 1 alone is 1
  CHECK(sentIs(out, 0, {0x1e}));
  const std::vector<uint8_t> ctrlEq0 = keys(kModRightCtrl | 0x02, {0x2e, 0x27, 0x04});
  fn.filter(ctrlEq0.data(), out);
  CHECK(sentIs(out, 0x02, {0x45, 0x43, 0x04}));
  const std::vector<uint8_t> leftCtrl1 = keys(0x01, {0x1e});   // Left Ctrl is no Fn
  fn.filter(none.data(), out);
  fn.filter(leftCtrl1.data(), out);
  CHECK(sentIs(out, 0x01, {0x1e}));
  // A detach forgets the keys: a rollover right after one sends nothing down.
  fn.reset();
  fn.filter(roll.data(), out);
  CHECK(sentIs(out, 0x01, {}));
}

// Swipes and taps as report sequences: X and Y in the touchpad's own counts, the contact count as the touchpad sends it.
static void report(uint8_t out[kTouchpadBytes], uint8_t buttons, std::initializer_list<Finger> fingers, int count = -1);
static void testSwipes()
{
  constexpr uint8_t kTouch = 1 | 2 | 4;
  uint8_t tp[kTouchpadBytes], out[2][kPointerBytes];
  // A one-finger swipe right: 300 X counts in 20 reports, 8 ms apart, two reports per frame as the cover may pack them.
  Pointer p;
  int32_t sumX = 0, sumY = 0;
  unsigned sent = 0;
  Report r[24];
  unsigned n = 0;
  for (unsigned i = 0; i <= 20; i += 2) {
    uint8_t a[kTouchpadBytes], b[kTouchpadBytes], f[kFrameBytes];
    report(a, 0, {{kTouch, 7, uint16_t(1000 + 15 * i), 1000}});
    report(b, 0, {{kTouch, 7, uint16_t(1000 + 15 * (i + 1)), 1000}});
    pack(f, uint8_t(i), {std::vector<uint8_t>(a, a + kTouchpadBytes), std::vector<uint8_t>(b, b + kTouchpadBytes)});
    CHECK(unpackFrame(f, r, 24, &n) == Frame::reports && n == 2 && r[0].length == kTouchpadBytes);
    for (unsigned k = 0; k < n; ++k) {
      const unsigned m = p.touch(r[k].bytes, 8 * (i + k), out);
      for (unsigned j = 0; j < m; ++j, ++sent) { sumX += px(out[j]); sumY += py(out[j]); CHECK(out[j][0] == 0); }
    }
  }
  report(tp, 0, {}, 0);
  CHECK(p.touch(tp, 200, out) == 0);
  CHECK(sumX == 315 / 2 && sumY == 0 && sent > 0);
  Gesture g;
  CHECK(p.takeGesture(&g) && g.most == 1 && g.x0 == 1000 && g.x1 == 1315 && g.travel == 315 && !g.tap && g.moves == sent);
  CHECK(!p.takeGesture(&g));
  // The same distance down in Y counts moves 2/3 as far (51 counts per mm against 34).
  Pointer q;
  sumY = 0;
  for (unsigned i = 0; i <= 30; ++i) {
    report(tp, 0, {{kTouch, 8, 1500, uint16_t(500 + 10 * i)}});
    const unsigned m = q.touch(tp, 8 * i, out);
    for (unsigned j = 0; j < m; ++j) sumY += py(out[j]);
  }
  CHECK(sumY == 300 * 2 / 3 / 2);
  // A two-finger swipe up: wheel steps (scroll up), no pointer motion; sideways: AC Pan.
  Pointer s;
  int wheel = 0, pan = 0, moved = 0;
  for (unsigned i = 0; i <= 10; ++i) {
    report(tp, 0, {{kTouch, 1, 1000, uint16_t(1500 - 30 * i)}, {kTouch, 2, 1400, uint16_t(1500 - 30 * i)}});
    const unsigned m = s.touch(tp, 8 * i, out);
    for (unsigned j = 0; j < m; ++j) { wheel += int8_t(out[j][5]); pan += int8_t(out[j][6]); moved += px(out[j]) != 0 || py(out[j]) != 0; }
  }
  CHECK(wheel == 4 && pan == 0 && moved == 0);   // 300 Y counts = 200 X counts: 4 steps of 50
  for (unsigned i = 0; i <= 10; ++i) {
    report(tp, 0, {{kTouch, 1, uint16_t(1000 + 20 * i), 1200}, {kTouch, 2, uint16_t(1400 + 20 * i), 1200}});
    const unsigned m = s.touch(tp, 100 + 8 * i, out);
    for (unsigned j = 0; j < m; ++j) pan += int8_t(out[j][6]);
  }
  CHECK(pan == 4);
  report(tp, 0, {}, 0);
  s.touch(tp, 300, out);
  CHECK(s.takeGesture(&g) && g.most == 2 && g.scrolls >= 6 && g.moves == 0);
  // The contact count rules: a slot past it is not a finger, even with its tip flag left set.
  Pointer c;
  report(tp, 0, {{kTouch, 3, 1000, 1000}, {kTouch, 4, 2000, 1000}}, 1);
  CHECK(c.touch(tp, 0, out) == 0);
  report(tp, 0, {{kTouch, 3, 1100, 1000}, {kTouch, 4, 2500, 1000}}, 1);   // one finger moving, a stale second slot
  CHECK(c.touch(tp, 8, out) == 1 && px(out[0]) == 50 && out[0][5] == 0);
  report(tp, 0, {{kTouch, 3, 1100, 1000}}, 0);                            // count 0: lifted, whatever the flags
  CHECK(c.touch(tp, 16, out) == 0 && c.takeGesture(&g) && g.most == 1);
  // Over three contacts: the next count-0 report is the same frame's continuation and changes nothing.
  report(tp, 0, {{kTouch, 1, 800, 900}, {kTouch, 2, 1200, 900}, {kTouch, 3, 1600, 900}}, 4);
  c.touch(tp, 100, out);
  report(tp, 0, {{kTouch, 4, 2000, 900}}, 0);
  CHECK(c.touch(tp, 101, out) == 0 && c.continuations() == 1 && !c.takeGesture(&g));
  report(tp, 0, {}, 0);
  c.touch(tp, 400, out);
  CHECK(c.takeGesture(&g) && g.most == 3);
  // A touchpad that never sends a count: the tip flags decide.
  Pointer z;
  report(tp, 0, {{kTouch, 5, 1000, 1000}}, 0);
  z.touch(tp, 0, out);
  report(tp, 0, {{kTouch, 5, 1040, 1000}}, 0);
  CHECK(z.touch(tp, 8, out) == 1 && px(out[0]) == 20);
  // A press started with two fingers stays the right button when one lifts; one started with one stays left.
  Pointer b;
  report(tp, 0, {{kTouch, 1, 1000, 1000}, {kTouch, 2, 1400, 1000}});
  b.touch(tp, 0, out);
  report(tp, 1, {{kTouch, 1, 1000, 1000}, {kTouch, 2, 1400, 1000}});
  CHECK(b.touch(tp, 10, out) == 1 && out[0][0] == 2);
  report(tp, 1, {{kTouch, 1, 1000, 1000}});
  CHECK(b.touch(tp, 20, out) == 0);                     // still the right button: nothing changes
  report(tp, 0, {{kTouch, 1, 1000, 1000}});
  CHECK(b.touch(tp, 30, out) == 1 && out[0][0] == 0);
  report(tp, 1, {{kTouch, 1, 1000, 1000}});
  CHECK(b.touch(tp, 40, out) == 1 && out[0][0] == 1);
  report(tp, 1, {{kTouch, 1, 1000, 1000}, {kTouch, 2, 1400, 1000}});
  CHECK(b.touch(tp, 50, out) == 0);                     // a second finger during a left press: still left
  report(tp, 2, {{kTouch, 1, 1000, 1000}});             // the second button: right
  CHECK(b.touch(tp, 60, out) == 1 && out[0][0] == 2);
  report(tp, 0, {}, 0);
  CHECK(b.touch(tp, 70, out) == 1 && out[0][0] == 0 && b.takeGesture(&g) && g.pressed && !g.tap);
  // A two-finger tap: the right button clicked; a reset (detach) keeps the counters.
  Pointer t;
  report(tp, 0, {{kTouch, 1, 1000, 1000}, {kTouch, 2, 1400, 1000}});
  t.touch(tp, 0, out);
  report(tp, 0, {}, 0);
  CHECK(t.touch(tp, 90, out) == 2 && out[0][0] == 2 && out[1][0] == 0 && t.takeGesture(&g) && g.tap && g.most == 2);
  report(tp, 0, {{kTouch, 1, 1000, 1000}});
  t.touch(tp, 200, out);
  t.reset();
  CHECK(t.taps() == 1 && t.gestures() == 1 && !t.takeGesture(&g));
  report(tp, 0, {{kTouch, 1, 1300, 1000}});              // after the reset the finger starts again: no jump
  CHECK(t.touch(tp, 210, out) == 0);
}
static void report(uint8_t out[kTouchpadBytes], uint8_t buttons, std::initializer_list<Finger> fingers, int count)
{
  touchpad(out, buttons, fingers);
  if (count >= 0) out[26] = uint8_t(count);
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
  testCaptured();
  testAttach();
  testAuth();
  testKeys();
  testPointer();
  testSwipes();
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
