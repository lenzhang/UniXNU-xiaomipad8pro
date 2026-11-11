// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Nanosic WN8030 keyboard-cover controller as piano (Xiaomi Pad 8 Pro) wires it: the message codec only (no I/O, no waits), so
// tests/keyboard_host.cpp runs all of it on the host.
//
// The controller is an MCU on I2C (client 0x4c on i2c@a98000, QUP1 SE6) whose boot ROM takes a RAM image (MCU_Upgrade.bin) over I2C at
// every power-up; the firmware then raises GPIO97 for each 68-byte frame it has and the host reads it from register 0x4c. Facts and
// their sources (package/PianoKeyboard/UPSTREAM):
//   boot ROM: commands {0x5c, cmd} (0x04 id, 0x05 state, 0x08 verify, 0x09 boot), the id/state read back from register 0x5c; id 0xc8,
//     states 0 idle, 1 header taken, 3 image verified; the header {0x5c, 0x06, ...65 bytes} and 503-byte data blocks
//     {0x5c, 0x07, offset BE32, length BE32, data, zero padding to 512}            (UniXNU-sheng Wn8030.c, Project-SunUEFI PianoPogoMcu.c)
//   runtime commands: 66 bytes {0x32, 0, 0x4e, 0x31, 0x80, 0x38, cmd, 1, value, sum8 of bytes 2..8, zeros} written to register 0x4c on
//     piano (0x5c on sheng)                                                       (SunUEFI PianoPogoMcu.c; the v1.1.0 Linux module, spec)
//   piano frames: byte 0 & 0x7f = 0x57, byte 2 a frame type (0 = nothing; 0x39, 0x4a, 0x5b or 0x6c), then reports packed from byte 3,
//     each sized by its ID: 0x02 mouse 8, 0x05 keyboard 9, 0x06 consumer 5, 0x19 touchpad 27, 0x22 vendor 16, 0x23 vendor 32, 0x24 and
//     0x26 vendor to the end of the frame; ID 0 ends the frame. A frame with reports means "read again".  (the v1.1.0 Linux module, spec)
//   reports: keyboard {5, modifiers, 0, six usages}; consumer {6, usage LE16, 2 more bytes}; mouse {2, buttons, X, Y, wheel: LE16
//     relative}; touchpad {0x19, buttons (bit 0 left, 1 right), three fingers of {flags (bit 0 tip, 1 in range, 2 confidence), contact
//     id, 2 unused bytes, X LE16 0..3199, Y LE16 0..2135}, contact count}   (the module's piano HID descriptors, decoded; spec)
//   vendor messages: {id, ?, 0x38, 0x80, kind, ...}: kind 0xa2 attach (byte 9 bit 0: the cover is on), 0x64 accelerometer, 0x24 / 0x31 /
//     0x32 authentication (left unanswered here, as UniXNU-sheng does: input works without it)
#ifndef PIANO_KEYBOARD_WN8030_H
#define PIANO_KEYBOARD_WN8030_H
#include <stddef.h>
#include <stdint.h>

namespace pianokeyboard {

constexpr uint8_t kAddress = 0x4c, kRegBoot = 0x5c, kRegRuntime = 0x4c;
constexpr unsigned kFrameBytes = 68;
constexpr uint8_t kBootId = 0xc8;
enum : uint8_t { kRomId = 0x04, kRomState = 0x05, kRomHeader = 0x06, kRomData = 0x07, kRomVerify = 0x08, kRomBoot = 0x09 };
enum : uint8_t { kStateIdle = 0, kStateHeader = 1, kStateVerified = 3 };

// The RAM image: "NANO IC" header at 0x7fc0 (code size LE32 at +48, its byte sum LE32 at +52), the code from 0x8000.
constexpr uint32_t kImageHeader = 0x7fc0, kImageCode = 0x8000, kImageMaxCode = 0x10000;
constexpr unsigned kHeaderWire = 66, kDataWire = 513, kDataChunk = 503, kCommandWire = 67;   // the register byte included

enum : uint8_t { kCmdTouchpad = 0x21, kCmdBacklight = 0x23, kCmdKeyboardPower = 0x25, kCmdIndicator = 0x2e };
enum : uint8_t { kLedCapsOn = 0xfd, kLedCapsOff = 0xfc, kLedMicOn = 0xf7, kLedMicOff = 0xf3 };

enum : uint8_t { kIdMouse = 0x02, kIdKeyboard = 0x05, kIdConsumer = 0x06, kIdTouchpad = 0x19 };
constexpr unsigned kKeyboardBytes = 9, kConsumerBytes = 3, kMouseBytes = 8, kTouchpadBytes = 27, kFingers = 3;
constexpr unsigned kTouchXMax = 3199, kTouchYMax = 2135;

inline uint8_t sum8(const uint8_t *p, unsigned n)
{
  uint8_t s = 0;
  for (unsigned i = 0; i < n; ++i) s = uint8_t(s + p[i]);
  return s;
}
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline void putLe16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void putLe32(uint8_t *p, uint32_t v) { putLe16(p, v); putLe16(p + 2, v >> 16); }
inline void putBe32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void zero(uint8_t *p, unsigned n) { for (unsigned i = 0; i < n; ++i) p[i] = 0; }

// ---------------------------------------------------------------------------------------------------------------- the RAM image
// 0 and the code size, or the failed check: -1 size, -2 magic, -3 code size (zero, over 64 KiB, not a whole word, past the file),
// -4 the header's byte sum of the code.
inline int imageCheck(const uint8_t *image, uint32_t size, uint32_t *codeBytes)
{
  static const uint8_t magic[7] = {'N', 'A', 'N', 'O', ' ', 'I', 'C'};
  if (!image || size < kImageCode + 4) return -1;
  const uint8_t *h = image + kImageHeader;
  for (unsigned i = 0; i < 7; ++i)
    if (h[i] != magic[i]) return -2;
  const uint32_t code = le32(h + 48);
  if (!code || code > kImageMaxCode || (code & 3) || code > size - kImageCode) return -3;
  uint32_t sum = 0;
  for (uint32_t i = 0; i < code; ++i) sum += image[kImageCode + i];
  if (sum != le32(h + 52)) return -4;
  *codeBytes = code;
  return 0;
}

// {0x5c, 0x06, magic[7], chip 0x8140, source 0x2345, destination 0x40, code size, entry, pid/vid, version, ..., the code's word sum
// at +53, the byte sum of bytes 1..63 at +64}: 66 bytes.
inline void headerCommand(uint8_t out[kHeaderWire], const uint8_t *image, uint32_t codeBytes)
{
  const uint8_t *h = image + kImageHeader;
  zero(out, kHeaderWire);
  out[0] = kRegBoot;
  uint8_t *c = out + 1;
  c[0] = kRomHeader;
  for (unsigned i = 0; i < 7; ++i) c[1 + i] = h[i];
  putLe16(c + 8, 0x8140);
  putLe32(c + 10, 0x2345);
  putLe32(c + 14, 0x40);
  putLe32(c + 18, codeBytes);
  for (unsigned i = 0; i < 4; ++i) c[22 + i] = h[21 + i];   // entry point
  for (unsigned i = 0; i < 4; ++i) c[26 + i] = h[25 + i];   // product and vendor id
  c[30] = h[29];
  c[31] = h[30];                                            // version
  uint32_t words = 0;
  for (uint32_t i = 0; i < codeBytes; i += 4) words += le32(image + kImageCode + i);
  putLe32(c + 53, words);
  c[64] = sum8(c + 1, 63);
}

// The data block for the code at offset: 513 bytes to write; returns the code bytes it carries (0: offset past the end).
inline unsigned dataCommand(uint8_t out[kDataWire], const uint8_t *image, uint32_t codeBytes, uint32_t offset)
{
  if (offset >= codeBytes) return 0;
  const unsigned n = codeBytes - offset < kDataChunk ? codeBytes - offset : kDataChunk;
  zero(out, kDataWire);
  out[0] = kRegBoot;
  out[1] = kRomData;
  putBe32(out + 2, offset);
  putBe32(out + 6, n);
  for (unsigned i = 0; i < n; ++i) out[10 + i] = image[kImageCode + offset + i];
  return n;
}

// A runtime command with its register byte: 67 bytes to write.
inline void command(uint8_t out[kCommandWire], uint8_t cmd, uint8_t value)
{
  zero(out, kCommandWire);
  out[0] = kRegRuntime;
  uint8_t *f = out + 1;
  f[0] = 0x32; f[2] = 0x4e; f[3] = 0x31; f[4] = 0x80; f[5] = 0x38;
  f[6] = cmd; f[7] = 1; f[8] = value;
  f[9] = sum8(f + 2, 7);
}

// ---------------------------------------------------------------------------------------------------------------------- frames
struct Report {
  uint8_t id, length;   // length in the frame, ID included
  const uint8_t *bytes;
};
enum class Frame : uint8_t { empty, reports, badMagic, badType, unknownId, overrun };
inline const char *frameName(Frame f)
{
  switch (f) {
  case Frame::empty: return "empty";
  case Frame::reports: return "reports";
  case Frame::badMagic: return "bad magic";
  case Frame::badType: return "bad frame type";
  case Frame::unknownId: return "unknown report id";
  case Frame::overrun: return "report past the frame";
  }
  return "?";
}

// The size of a report in a piano frame by its ID (0: not a known ID); rest is what is left of the frame.
inline unsigned reportSize(uint8_t id, unsigned rest)
{
  switch (id) {
  case kIdMouse: return kMouseBytes;
  case kIdKeyboard: return kKeyboardBytes;
  case kIdConsumer: return 5;
  case kIdTouchpad: return kTouchpadBytes;
  case 0x22: return 16;
  case 0x23: return 32;
  case 0x24: case 0x26: return rest;
  default: return 0;
  }
}

// Splits one frame into its reports (at most capacity; a frame holds at most 21 of the smallest).
inline Frame unpackFrame(const uint8_t frame[kFrameBytes], Report *out, unsigned capacity, unsigned *count)
{
  *count = 0;
  if (frame[0] == 0x00 || frame[0] == 0xff) return Frame::empty;   // nothing pending (a read without an edge)
  if ((frame[0] & 0x7f) != 0x57) return Frame::badMagic;
  const uint8_t type = frame[2];
  if (!type) return Frame::empty;
  if (type != 0x39 && type != 0x4a && type != 0x5b && type != 0x6c) return Frame::badType;
  for (unsigned at = 3; at < kFrameBytes && frame[at];) {
    const unsigned size = reportSize(frame[at], kFrameBytes - at);
    if (!size) return Frame::unknownId;
    if (size > kFrameBytes - at) return Frame::overrun;
    if (*count < capacity) out[(*count)++] = {frame[at], uint8_t(size), frame + at};
    at += size;
  }
  return *count ? Frame::reports : Frame::empty;
}

enum class Vendor : uint8_t { none, attach, accel, authRequest, authInit, authChallenge, other };
inline const char *vendorName(Vendor v)
{
  switch (v) {
  case Vendor::none: return "-";
  case Vendor::attach: return "attach";
  case Vendor::accel: return "accelerometer";
  case Vendor::authRequest: return "auth request";
  case Vendor::authInit: return "auth init";
  case Vendor::authChallenge: return "auth challenge";
  case Vendor::other: return "other";
  }
  return "?";
}
// A vendor message (IDs 0x22, 0x23, 0x24, 0x26): its kind and, for an attach message, the state (1 on, 0 off).
inline Vendor vendorKind(const Report &r, int *attached)
{
  *attached = -1;
  if (r.id != 0x22 && r.id != 0x23 && r.id != 0x24 && r.id != 0x26) return Vendor::none;
  if (r.length < 10 || r.bytes[2] != 0x38 || r.bytes[3] != 0x80) return Vendor::other;
  switch (r.bytes[4]) {
  case 0xa2: *attached = r.bytes[9] & 1; return Vendor::attach;
  case 0x64: return Vendor::accel;
  case 0x24: return Vendor::authRequest;
  case 0x31: return Vendor::authInit;
  case 0x32: return Vendor::authChallenge;
  default: return Vendor::other;
  }
}

// ------------------------------------------------------------------------------------------------------- the pointer for macOS
// macOS's HID event driver makes no pointer events of a touchpad's finger collections (UniXNU-sheng KeyboardCover.cpp: only Apple's
// own trackpads have a driver), so the touchpad becomes a relative mouse (HidDescriptors.h kPointerDescriptor, 7 bytes, no report ID):
// {buttons (bit 0 left, 1 right), X LE16, Y LE16, wheel, AC pan}. One finger moves; two or more scroll; the clickpad's press is the
// left button, with two fingers down the right one; a short light touch is a click (tap). A touching contact without confidence (a
// palm) counts as lifted. Design from UniXNU-sheng Wn8030.c wn8030_pointer_report, for piano's 8-byte fingers, with taps.
constexpr unsigned kPointerBytes = 7;
struct PointerConfig {
  int32_t divide = 2;         // touchpad counts per pointer count (3200 counts over ~94 mm: 34 per mm)
  int32_t scrollCounts = 50;  // counts of finger travel per wheel step (~1.5 mm)
  bool tap = true;
  bool invertX = false, invertY = false;
};
class Pointer {
 public:
  explicit Pointer(const PointerConfig &c = PointerConfig()) : cfg_(c) {}
  void configure(const PointerConfig &c) { cfg_ = c; }
  // A touchpad report: up to two pointer reports (a tap: press, then release) into out; returns how many.
  unsigned touch(const uint8_t *tp, uint64_t ms, uint8_t out[2][kPointerBytes])
  {
    unsigned fingers = 0;
    uint8_t contact = 0;
    int32_t x = 0, y = 0;
    for (unsigned f = 0; f < kFingers; ++f) {
      const uint8_t *p = tp + 2 + 8 * f;
      if ((p[0] & 1) == 0 || (p[0] & 4) == 0) continue;   // lifted, or a palm
      if (!fingers) contact = p[1];
      x += le16(p + 4);
      y += le16(p + 6);
      ++fingers;
    }
    if (fingers) { x /= int32_t(fingers); y /= int32_t(fingers); }
    if (cfg_.invertX) x = int32_t(kTouchXMax) - x;
    if (cfg_.invertY) y = int32_t(kTouchYMax) - y;
    // A finger that came, went or changed places starts from where it is: no jump.
    if (fingers && fingers == fingers_ && (fingers > 1 || contact == contact_)) {
      const int32_t dx = x - x_, dy = y - y_;
      if (fingers == 1) { moveX_ += dx; moveY_ += dy; }
      else { scrollX_ += dx; scrollY_ -= dy; }
      travel_ += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    }
    if (fingers != fingers_) moveX_ = moveY_ = scrollX_ = scrollY_ = 0;
    // Taps: a touch of at most 180 ms, 2.5 mm of travel and no press; its most fingers say which button.
    if (fingers && !fingers_ && !touching_) { touching_ = true; downMs_ = ms; travel_ = 0; most_ = 0; pressed_ = false; }
    if (fingers > most_) most_ = uint8_t(fingers);
    if (tp[1] & 3) pressed_ = true;
    unsigned n = 0;
    uint8_t buttons = 0;
    if (tp[1] & 1) buttons |= fingers >= 2 ? 2 : 1;
    if (tp[1] & 2) buttons |= 2;
    const bool tapped = !fingers && touching_ && cfg_.tap && !pressed_ && ms - downMs_ <= 180 && travel_ <= 85;
    if (!fingers) touching_ = false;
    fingers_ = uint8_t(fingers);
    contact_ = contact;
    x_ = x;
    y_ = y;
    const int32_t mx = take(&moveX_, cfg_.divide, 32767), my = take(&moveY_, cfg_.divide, 32767);
    const int32_t sy = take(&scrollY_, cfg_.scrollCounts, 127), sx = take(&scrollX_, cfg_.scrollCounts, 127);
    if (mx || my || sy || sx || buttons != buttons_) {
      buttons_ = buttons;
      put(out[n++], buttons, mx, my, sy, sx);
    }
    if (tapped && !buttons) {
      const uint8_t click = most_ >= 2 ? 2 : 1;
      n = 0;   // nothing moves at a lift: the tap's two reports are all
      put(out[n++], click, 0, 0, 0, 0);
      put(out[n++], 0, 0, 0, 0, 0);
      ++taps_;
    }
    return n;
  }
  // The MCU's own relative mouse report (ID 2): buttons, X, Y, wheel.
  static void mouse(const uint8_t *m, uint8_t out[kPointerBytes])
  {
    const int32_t wheel = int16_t(le16(m + 6));
    put(out, m[1] & 7, int16_t(le16(m + 2)), int16_t(le16(m + 4)), wheel > 127 ? 127 : wheel < -127 ? -127 : wheel, 0);
  }
  static void release(uint8_t out[kPointerBytes]) { put(out, 0, 0, 0, 0, 0); }
  void reset() { *this = Pointer(cfg_); }
  uint32_t taps() const { return taps_; }

 private:
  static int32_t take(int32_t *pending, int32_t unit, int32_t limit)
  {
    int32_t steps = *pending / unit;
    if (steps > limit) steps = limit;
    if (steps < -limit) steps = -limit;
    *pending -= steps * unit;
    return steps;
  }
  static void put(uint8_t *o, uint8_t buttons, int32_t x, int32_t y, int32_t wheel, int32_t pan)
  {
    o[0] = buttons;
    putLe16(o + 1, uint16_t(int16_t(x)));
    putLe16(o + 3, uint16_t(int16_t(y)));
    o[5] = uint8_t(int8_t(wheel));
    o[6] = uint8_t(int8_t(pan));
  }
  PointerConfig cfg_;
  uint8_t fingers_ = 0, contact_ = 0, buttons_ = 0, most_ = 0;
  bool touching_ = false, pressed_ = false;
  int32_t x_ = 0, y_ = 0, moveX_ = 0, moveY_ = 0, scrollX_ = 0, scrollY_ = 0, travel_ = 0;
  uint64_t downMs_ = 0;
  uint32_t taps_ = 0;
};

}  // namespace pianokeyboard
#endif
