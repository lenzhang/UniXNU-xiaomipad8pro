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
//   frame trailer: byte 67 = the byte sum of bytes 2..66, and a vendor message {id, ?, 0x38, 0x80, kind, length, payload} ends in the
//     byte sum of its bytes 0..5+length (both seen on every captured frame, 2026-11-11; Linux checks neither, nor does this driver:
//     a mismatch is only counted, frameSumOk)
//   reports: keyboard {5, modifiers, 0, six usages}; consumer {6, usage LE16, 2 more bytes}; mouse {2, buttons, X, Y, wheel: LE16
//     relative}; touchpad {0x19, buttons (bit 0 left, 1 right), three fingers of {flags (bit 0 tip, 1 in range, 2 confidence), contact
//     id, 2 unused bytes, X LE16 0..3199, Y LE16 0..2135}, contact count}   (the module's piano HID descriptors, decoded; spec)
//   vendor messages: {id, ?, 0x38, 0x80, kind, length, payload...}, at least 12 bytes: kind 0xa2 attach (byte 9 bit 0: the cover is
//     on, as the v1.1.0 module reads it; captured 0x23), 0x64 accelerometer, 0x24 / 0x31 / 0x32 authentication (0x24's byte 6: 0x00
//     or 0x01 a first request, 0x64 a repeated one; unanswered by default, as piano's Ubuntu and UniXNU-sheng run; authInit/authS3t1
//     below on request). Seen on piano 2026-11-11 (logs/usb-console/20261111-084701-port0.txt), meaning inferred: after each
//     command the cover sends kind 0xf0 {command, 0} (an acknowledgement) and then the command's own kind {value} (its state:
//     0x21 01, 0x25 01, 0x2e fc); 0x28 {01} with each keyboard-power command; 0xa4 and 0x01 (byte 1 0x30) before and after attach
//   the keyboard report goes to the host as the controller sends it, except: an ErrorRollOver report (a key slot 1..3) keeps the
//     previous keys (what Linux's HID core does with it), and piano-keyboard-fnrow=1 makes Right Ctrl + 1..0 - = into F1..F12 (the
//     newer Linux port's "missing Fn layer"; v1.1.0 passes the keys through, which is the default here)
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

// A runtime message with its register byte, 67 bytes to write: {0x32, 0, channel, 0x31, 0x80, 0x38, cmd, length, payload, the byte
// sum of bytes 2.. up to the payload's end}. Channel 0x4e carries the control commands, 0x4f the authentication messages. Returns
// false for a payload over 57 bytes.
constexpr uint8_t kChannelControl = 0x4e, kChannelAuth = 0x4f;
inline bool message(uint8_t out[kCommandWire], uint8_t channel, uint8_t cmd, const uint8_t *payload, unsigned length)
{
  if (length > kCommandWire - 10) return false;
  zero(out, kCommandWire);
  out[0] = kRegRuntime;
  uint8_t *f = out + 1;
  f[0] = 0x32; f[2] = channel; f[3] = 0x31; f[4] = 0x80; f[5] = 0x38;
  f[6] = cmd; f[7] = uint8_t(length);
  for (unsigned i = 0; i < length; ++i) f[8 + i] = payload[i];
  f[8 + length] = sum8(f + 2, 6 + length);
  return true;
}
// A control command of one value byte.
inline void command(uint8_t out[kCommandWire], uint8_t cmd, uint8_t value) { message(out, kChannelControl, cmd, &value, 1); }

// Authentication, as the Linux driver answers when its userspace helper has /dev/nanosic_auth open (without it, as on piano's Ubuntu
// v1.1.0, it answers nothing): the cover's request (vendor kind 0x24) gets the init {0x31, "MIAUTH"}; the cover's init reply (0x31,
// its 16-byte UID at +8) gets {0x32: key meta 00 00 00 02, a fixed 16-byte host challenge} ("offline auth"); the cover's challenge
// (0x32, 16 bytes at +22) needs a 16-byte token {0x33} that only the helper computes (a secret this driver does not have).
enum : uint8_t { kAuthInit = 0x31, kAuthS3t1 = 0x32, kAuthS5t1 = 0x33 };
inline void authInit(uint8_t out[kCommandWire])
{
  static const uint8_t miauth[6] = {'M', 'I', 'A', 'U', 'T', 'H'};
  message(out, kChannelAuth, kAuthInit, miauth, sizeof miauth);
}
inline void authS3t1(uint8_t out[kCommandWire])
{
  static const uint8_t payload[20] = {0x00, 0x00, 0x00, 0x02, 0x80, 0x3d, 0x84, 0x36, 0x29, 0x82, 0xde, 0x6a,
                                      0x0e, 0x68, 0x1d, 0x3d, 0x6b, 0x32, 0xa5, 0xa6};
  message(out, kChannelAuth, kAuthS3t1, payload, sizeof payload);
}

// The control commands sent when the cover attaches, in order: keyboard power (as every 12 s after), the Caps Lock LED as macOS
// has it, the touchpad on (value; 2: not sent), the mic LED off, the backlight 0..100 (over 100: not sent). The v1.1.0 module's
// attach and input-state work send the same set (touchpad, Caps, mic, backlight; keyboard power from its 12 s worker); keyboard
// power first is what piano ran on 2026-11-11 (each acknowledged, kind 0xf0).
struct Command {
  uint8_t cmd, value;
  const char *what;
};
inline unsigned attachCommands(bool caps, uint32_t touchpad, uint32_t backlight, Command out[5])
{
  unsigned n = 0;
  out[n++] = {kCmdKeyboardPower, 1, "keyboard power"};
  out[n++] = {kCmdIndicator, caps ? kLedCapsOn : kLedCapsOff, "caps lock LED"};
  if (touchpad != 2) out[n++] = {kCmdTouchpad, uint8_t(touchpad), "touchpad"};
  out[n++] = {kCmdIndicator, kLedMicOff, "mic LED"};
  if (backlight <= 100) out[n++] = {kCmdBacklight, uint8_t(backlight), "backlight"};
  return n;
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

// The frame's trailer checksum (byte 67 = sum8 of bytes 2..66), seen on the captured frames; not a condition for anything.
inline bool frameSumOk(const uint8_t frame[kFrameBytes]) { return sum8(frame + 2, kFrameBytes - 3) == frame[kFrameBytes - 1]; }

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

enum class Vendor : uint8_t { none, attach, accel, authRequest, authInit, authChallenge, ack, state, other };
inline const char *vendorName(Vendor v)
{
  switch (v) {
  case Vendor::none: return "-";
  case Vendor::attach: return "attach";
  case Vendor::accel: return "accelerometer";
  case Vendor::authRequest: return "auth request";
  case Vendor::authInit: return "auth init";
  case Vendor::authChallenge: return "auth challenge";
  case Vendor::ack: return "command ack";
  case Vendor::state: return "command state";
  case Vendor::other: return "other";
  }
  return "?";
}
// A vendor message (IDs 0x22, 0x23, 0x24, 0x26): its kind and, for an attach message, the state (1 on, 0 off). Under 12 bytes or
// without the 0x38 0x80 route it is "other" (Linux ignores those).
inline Vendor vendorKind(const Report &r, int *attached)
{
  *attached = -1;
  if (r.id != 0x22 && r.id != 0x23 && r.id != 0x24 && r.id != 0x26) return Vendor::none;
  if (r.length < 12 || r.bytes[2] != 0x38 || r.bytes[3] != 0x80) return Vendor::other;
  switch (r.bytes[4]) {
  case 0xa2: *attached = r.bytes[9] & 1; return Vendor::attach;
  case 0x64: return Vendor::accel;
  case 0x24: return Vendor::authRequest;
  case 0x31: return Vendor::authInit;
  case 0x32: return Vendor::authChallenge;
  case 0xf0: return Vendor::ack;
  case kCmdTouchpad: case kCmdBacklight: case kCmdKeyboardPower: case kCmdIndicator: return Vendor::state;
  default: return Vendor::other;
  }
}
// Whether a vendor message carries authentication material (the cover's UID, a challenge): never logged in full.
inline bool authPayload(const Report &r)
{
  int attached;
  const Vendor v = vendorKind(r, &attached);
  return v == Vendor::authInit || v == Vendor::authChallenge;
}

// piano-keyboard-auth=1: the answer to an authentication message, as the Linux driver gives it with its helper open. A request
// (0x24, any value) gets the init; the cover's init reply (0x31, its UID at +8: at least 24 bytes) gets the offline s3t1; the
// cover's challenge (0x32, 16 bytes at +22: at least 38) would need the helper's token, which is not sent.
enum class AuthStep : uint8_t { none, init, s3t1, challenge, tooShort };
inline AuthStep authReply(const Report &r, uint8_t wire[kCommandWire])
{
  int attached;
  switch (vendorKind(r, &attached)) {
  case Vendor::authRequest: authInit(wire); return AuthStep::init;
  case Vendor::authInit:
    if (r.length < 24) return AuthStep::tooShort;
    authS3t1(wire);
    return AuthStep::s3t1;
  case Vendor::authChallenge: return r.length < 38 ? AuthStep::tooShort : AuthStep::challenge;
  default: return AuthStep::none;
  }
}

// ---------------------------------------------------------------------------------------------------------- the keyboard report
// What goes to macOS for each controller keyboard report {5, modifiers, 0, six usages}: the same 9 bytes, except
//   - a report with ErrorRollOver (or POST Fail / Undefined: usages 1..3) in a key slot keeps the keys last sent, with the new
//     modifiers: the controller has more keys down than it can list, and the previous set is the best known state (Linux's HID core
//     ignores such an array the same way; macOS would see every held key released and pressed again);
//   - with fnRow: Right Ctrl (modifier bit 4) held makes 1..0, -, = (usages 0x1e..0x27, 0x2d, 0x2e) into F1..F12 (0x3a..0x45). A key
//     so made stays an F key until it is released, whatever Ctrl does meanwhile, and Right Ctrl is not reported while any is held
//     (no stray digit, no stuck F key, no Ctrl+F chord).
constexpr uint8_t kModRightCtrl = 1u << 4;
class Keys {
 public:
  bool fnRow = false;
  void filter(const uint8_t *in, uint8_t out[kKeyboardBytes])
  {
    for (unsigned i = 0; i < kKeyboardBytes; ++i) out[i] = in[i];
    bool rollover = false;
    for (unsigned i = 3; i < kKeyboardBytes; ++i) rollover = rollover || (in[i] >= 1 && in[i] <= 3);
    if (rollover) {
      ++rollovers_;
      for (unsigned i = 0; i < 6; ++i) out[3 + i] = sent_[i];
    } else if (fnRow) {
      uint16_t down = 0;
      for (unsigned i = 3; i < kKeyboardBytes; ++i) {
        const int slot = rowSlot(in[i]);
        if (slot < 0) continue;
        const uint16_t bit = uint16_t(1u << slot);
        down |= bit;
        if ((in[1] & kModRightCtrl) || (fn_ & bit)) {
          fn_ |= bit;
          out[i] = uint8_t(0x3a + slot);
        }
      }
      fn_ &= down;
    }
    if (fn_) out[1] &= uint8_t(~kModRightCtrl);
    for (unsigned i = 0; i < 6; ++i) sent_[i] = out[3 + i];
  }
  void reset() { for (uint8_t &k : sent_) k = 0; fn_ = 0; }
  uint32_t rollovers() const { return rollovers_; }
  uint16_t fnHeld() const { return fn_; }

 private:
  static int rowSlot(uint8_t usage)
  {
    if (usage >= 0x1e && usage <= 0x27) return usage - 0x1e;   // 1..9, 0
    if (usage == 0x2d) return 10;                              // -
    if (usage == 0x2e) return 11;                              // =
    return -1;
  }
  uint8_t sent_[6] = {};
  uint16_t fn_ = 0;
  uint32_t rollovers_ = 0;
};

// ------------------------------------------------------------------------------------------------------- the pointer for macOS
// macOS's HID event driver makes no pointer events of a touchpad's finger collections (UniXNU-sheng KeyboardCover.cpp: only Apple's
// own trackpads have a driver), so the touchpad becomes a relative mouse (HidDescriptors.h kPointerDescriptor, 7 bytes, no report ID):
// {buttons (bit 0 left, 1 right), X LE16, Y LE16, wheel, AC pan}. One finger moves; two or more scroll; the clickpad's press is the
// left button, or the right one when it starts with two fingers down (kept until the press ends); its second button is the right
// one; a short light touch is a click (tap). A touching contact without confidence (a palm) counts as lifted. Design from
// UniXNU-sheng Wn8030.c wn8030_pointer_report, for piano's 8-byte fingers, with taps.
// Which contacts a report holds: the first `contact count` (byte 26) of its three slots, as Linux's hid-multitouch reads this
// touchpad (a Windows-8 digitizer: the count is accurate, a contact not reported is lifted); a count of 0 right after one over three
// is that frame's continuation and changes nothing. Until the touchpad has sent a non-zero count the tip flags alone decide.
// The axes have different resolutions (the touchpad's HID descriptor: X 0..3199 over 94 mm, Y 0..2135 over 42 mm, 34 and 51 counts
// per mm), so Y counts are scaled by 2/3 into X counts before anything else: a millimetre is the same on both axes.
constexpr unsigned kPointerBytes = 7;
constexpr int32_t kYNum = 2, kYDen = 3;
struct PointerConfig {
  int32_t divide = 2;         // touchpad X counts per pointer count (3200 counts over ~94 mm: 34 per mm)
  int32_t scrollCounts = 50;  // X counts of finger travel per wheel step (~1.5 mm)
  bool tap = true;
  bool invertX = false, invertY = false;
};
// One touch from the first finger down to the last one up, for the log.
struct Gesture {
  uint32_t ms, travel, moves, scrolls;   // duration; finger travel in X counts; pointer reports that moved, that scrolled
  int32_t x0, y0, x1, y1;                // where it started and ended (touchpad counts; the fingers' mean)
  uint8_t most;                          // most fingers at once
  bool pressed, tap;                     // the pad was pressed; it was a tap (click sent)
};
class Pointer {
 public:
  explicit Pointer(const PointerConfig &c = PointerConfig()) : cfg_(c) {}
  void configure(const PointerConfig &c) { cfg_ = c; }
  // A touchpad report: up to two pointer reports (a tap: press, then release) into out; returns how many.
  unsigned touch(const uint8_t *tp, uint64_t ms, uint8_t out[2][kPointerBytes])
  {
    const uint8_t count = tp[26];
    if (!count && continuation_) {
      continuation_ = false;
      ++continuations_;
      return 0;
    }
    continuation_ = count > kFingers;
    if (count) countSeen_ = true;
    const unsigned slots = count ? (count < kFingers ? count : kFingers) : (countSeen_ ? 0 : kFingers);
    unsigned fingers = 0;
    uint8_t contact = 0;
    int32_t x = 0, y = 0;
    for (unsigned f = 0; f < slots; ++f) {
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
      if (fingers == 1) { moveX_ += dx; moveY_ += kYNum * dy; }
      else { scrollX_ += dx; scrollY_ -= kYNum * dy; }
      travel_ += uint32_t((dx < 0 ? -dx : dx) + ((dy < 0 ? -dy : dy) * kYNum + kYDen / 2) / kYDen);
    }
    if (fingers != fingers_) moveX_ = moveY_ = scrollX_ = scrollY_ = 0;
    // Taps: a touch of at most 180 ms, 2.5 mm of travel and no press; its most fingers say which button.
    if (fingers && !fingers_ && !touching_) {
      touching_ = true;
      downMs_ = ms;
      travel_ = 0;
      most_ = 0;
      pressed_ = false;
      g_ = Gesture();
      g_.x0 = x;
      g_.y0 = y;
    }
    if (fingers) { g_.x1 = x; g_.y1 = y; }
    if (fingers > most_) most_ = uint8_t(fingers);
    if (tp[1] & 3) pressed_ = true;
    // The press (button 1): which button it is is decided when it starts.
    const bool press = tp[1] & 1;
    if (press && !pressing_) pressButton_ = fingers >= 2 ? 2 : 1;
    pressing_ = press;
    uint8_t buttons = press ? pressButton_ : 0;
    if (tp[1] & 2) buttons |= 2;
    const bool tapped = !fingers && touching_ && cfg_.tap && !pressed_ && ms - downMs_ <= 180 && travel_ <= 85;
    if (!fingers && touching_) {
      touching_ = false;
      g_.ms = uint32_t(ms - downMs_);
      g_.travel = travel_;
      g_.most = most_;
      g_.pressed = pressed_;
      g_.tap = tapped && !buttons;
      ++gestures_;
      gestureDone_ = true;
    }
    fingers_ = uint8_t(fingers);
    contact_ = contact;
    x_ = x;
    y_ = y;
    unsigned n = 0;
    const int32_t mx = take(&moveX_, cfg_.divide, 32767), my = take(&moveY_, kYDen * cfg_.divide, 32767);
    const int32_t sy = take(&scrollY_, kYDen * cfg_.scrollCounts, 127), sx = take(&scrollX_, cfg_.scrollCounts, 127);
    if (mx || my || sy || sx || buttons != buttons_) {
      buttons_ = buttons;
      put(out[n++], buttons, mx, my, sy, sx);
      if (mx || my) ++g_.moves;
      if (sx || sy) ++g_.scrolls;
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
  // The touch that just ended (once per touch), for the log.
  bool takeGesture(Gesture *g)
  {
    if (!gestureDone_) return false;
    gestureDone_ = false;
    *g = g_;
    return true;
  }
  // The MCU's own relative mouse report (ID 2): buttons, X, Y, wheel.
  static void mouse(const uint8_t *m, uint8_t out[kPointerBytes])
  {
    const int32_t wheel = int16_t(le16(m + 6));
    put(out, m[1] & 7, int16_t(le16(m + 2)), int16_t(le16(m + 4)), wheel > 127 ? 127 : wheel < -127 ? -127 : wheel, 0);
  }
  static void release(uint8_t out[kPointerBytes]) { put(out, 0, 0, 0, 0, 0); }
  // A detach: nothing held, nothing pending; the counters stay.
  void reset()
  {
    const uint32_t taps = taps_, gestures = gestures_, continuations = continuations_;
    const bool countSeen = countSeen_;
    *this = Pointer(cfg_);
    taps_ = taps;
    gestures_ = gestures;
    continuations_ = continuations;
    countSeen_ = countSeen;
  }
  uint32_t taps() const { return taps_; }
  uint32_t gestures() const { return gestures_; }
  uint32_t continuations() const { return continuations_; }

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
  uint8_t fingers_ = 0, contact_ = 0, buttons_ = 0, most_ = 0, pressButton_ = 1;
  bool touching_ = false, pressed_ = false, pressing_ = false, continuation_ = false, countSeen_ = false, gestureDone_ = false;
  int32_t x_ = 0, y_ = 0, moveX_ = 0, moveY_ = 0, scrollX_ = 0, scrollY_ = 0;
  uint32_t travel_ = 0;
  uint64_t downMs_ = 0;
  uint32_t taps_ = 0, gestures_ = 0, continuations_ = 0;
  Gesture g_ = {};
};

}  // namespace pianokeyboard
#endif
