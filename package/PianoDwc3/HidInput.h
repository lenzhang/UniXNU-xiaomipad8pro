// SPDX-License-Identifier: BSD-2-Clause-Patent
// The input port's command protocol (PianoDwc3 port 2, piano-usb-hid=1): the Mac (tooling/native/piano-input.py) sends one ASCII
// command per line; each is answered with one line that never repeats what was sent. The commands are HID reports in the layouts of
// package/PianoKeyboard/HidDescriptors.h, which the two PianoUsbHID devices (keyboard, pointer) declare:
//   K <mods> [<usage> ...]          keyboard report 5: modifier bits (hex, bit 0 left control .. bit 7 right GUI) and up to six
//                                   key usages (hex, page 7); "K 0" releases every key
//   C <usage>                       consumer report 6: one usage (hex, page 0x0C, <= 0x3ff); "C 0" releases it
//   P <buttons> <dx> <dy> [<wheel> [<pan>]]   pointer report: buttons (hex, bit 0 left, 1 right, 2 middle), relative X/Y
//                                   (decimal, -32767..32767), wheel and AC pan (decimal, -127..127)
//   R                               release everything (keyboard, consumer, buttons)
//   ?                               status line
// Replies: "ok <n>" (n counts accepted commands), "err <what>" (bad command; nothing sent), "status ...". Lines are at most
// kLineMax bytes; '\r' or '\n' ends one; a longer line is dropped whole ("err long").
// Portable (no kernel headers): built into the kext and into tests/hid_input_host.cpp.
#pragma once
#include <stdint.h>

namespace pianousb {

constexpr unsigned kLineMax = 128;
constexpr unsigned kKeyboardReportBytes = 9, kConsumerReportBytes = 3, kPointerReportBytes = 7;   // = pianokeyboard's sizes
constexpr uint8_t kReportKeyboard = 0x05, kReportConsumer = 0x06;                                 // = pianokeyboard's report IDs

enum class HidOp : uint8_t { none, keyboard, consumer, pointer, release, status, error };

struct HidCommand {
  HidOp op = HidOp::none;
  uint8_t report[kKeyboardReportBytes] = {};
  unsigned bytes = 0;
  const char *error = nullptr;
  bool keysDown = false;   // the report holds a key, a consumer usage or a button (for the auto-release)
};

// Bytes in, whole lines out (no echo, no editing). push() returns 1 for a complete line (in line()), -1 for a dropped one.
class LineAssembler {
public:
  int push(char c)
  {
    if (c == '\r' || c == '\n') {
      const bool dropped = overflow_;
      overflow_ = false;
      if (dropped) { length_ = 0; return -1; }
      if (!length_) return 0;
      line_[length_] = 0;
      length_ = 0;
      return 1;
    }
    if (overflow_) return 0;
    if (length_ + 1 >= kLineMax) { overflow_ = true; return 0; }
    line_[length_++] = c;
    return 0;
  }
  const char *line() const { return line_; }

private:
  char line_[kLineMax] = {};
  unsigned length_ = 0;
  bool overflow_ = false;
};

namespace hidparse {
inline bool space(char c) { return c == ' ' || c == '\t'; }
inline const char *skip(const char *s) { while (space(*s)) ++s; return s; }
// One hex field; false when s holds none or it is too long.
inline bool hex(const char *&s, uint32_t *out)
{
  s = skip(s);
  uint32_t v = 0;
  unsigned digits = 0;
  for (;; ++s) {
    const char c = *s;
    unsigned d;
    if (c >= '0' && c <= '9') d = unsigned(c - '0');
    else if (c >= 'a' && c <= 'f') d = unsigned(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = unsigned(c - 'A' + 10);
    else break;
    if (++digits > 8) return false;
    v = v << 4 | d;
  }
  if (!digits || (*s && !space(*s))) return false;
  *out = v;
  return true;
}
inline bool dec(const char *&s, int32_t *out)
{
  s = skip(s);
  bool negative = false;
  if (*s == '-' || *s == '+') negative = *s++ == '-';
  int64_t v = 0;
  unsigned digits = 0;
  for (; *s >= '0' && *s <= '9'; ++s) {
    if (++digits > 9) return false;
    v = v * 10 + (*s - '0');
  }
  if (!digits || (*s && !space(*s))) return false;
  *out = int32_t(negative ? -v : v);
  return true;
}
inline bool end(const char *s) { return !*skip(s); }
}  // namespace hidparse

inline HidCommand parseHidCommand(const char *line)
{
  using namespace hidparse;
  HidCommand c;
  const char *s = skip(line);
  const char verb = *s;
  if (verb) ++s;
  if (verb && *s && !space(*s)) { c.op = HidOp::error; c.error = "verb"; return c; }
  switch (verb) {
  case 'K': {
    uint32_t mods = 0;
    if (!hex(s, &mods) || mods > 0xff) { c.op = HidOp::error; c.error = "modifiers"; return c; }
    c.report[0] = kReportKeyboard;
    c.report[1] = uint8_t(mods);
    unsigned keys = 0;
    while (!end(s)) {
      uint32_t usage = 0;
      if (keys == 6) { c.op = HidOp::error; c.error = "more than six keys"; return c; }
      if (!hex(s, &usage) || usage > 0xff) { c.op = HidOp::error; c.error = "key usage"; return c; }
      c.report[3 + keys++] = uint8_t(usage);
    }
    c.op = HidOp::keyboard;
    c.bytes = kKeyboardReportBytes;
    c.keysDown = mods || keys;
    return c;
  }
  case 'C': {
    uint32_t usage = 0;
    if (!hex(s, &usage) || usage > 0x3ff || !end(s)) { c.op = HidOp::error; c.error = "consumer usage"; return c; }
    c.report[0] = kReportConsumer;
    c.report[1] = uint8_t(usage);
    c.report[2] = uint8_t(usage >> 8);
    c.op = HidOp::consumer;
    c.bytes = kConsumerReportBytes;
    c.keysDown = usage != 0;
    return c;
  }
  case 'P': {
    uint32_t buttons = 0;
    int32_t dx = 0, dy = 0, wheel = 0, pan = 0;
    if (!hex(s, &buttons) || buttons > 7) { c.op = HidOp::error; c.error = "buttons"; return c; }
    if (!dec(s, &dx) || !dec(s, &dy) || dx < -32767 || dx > 32767 || dy < -32767 || dy > 32767) {
      c.op = HidOp::error; c.error = "motion"; return c;
    }
    if (!end(s) && (!dec(s, &wheel) || wheel < -127 || wheel > 127)) { c.op = HidOp::error; c.error = "wheel"; return c; }
    if (!end(s) && (!dec(s, &pan) || pan < -127 || pan > 127)) { c.op = HidOp::error; c.error = "pan"; return c; }
    if (!end(s)) { c.op = HidOp::error; c.error = "trailing"; return c; }
    c.report[0] = uint8_t(buttons);
    c.report[1] = uint8_t(uint16_t(dx));
    c.report[2] = uint8_t(uint16_t(dx) >> 8);
    c.report[3] = uint8_t(uint16_t(dy));
    c.report[4] = uint8_t(uint16_t(dy) >> 8);
    c.report[5] = uint8_t(int8_t(wheel));
    c.report[6] = uint8_t(int8_t(pan));
    c.op = HidOp::pointer;
    c.bytes = kPointerReportBytes;
    c.keysDown = buttons != 0;
    return c;
  }
  case 'R':
    if (!end(s)) { c.op = HidOp::error; c.error = "trailing"; return c; }
    c.op = HidOp::release;
    return c;
  case '?':
    if (!end(s)) { c.op = HidOp::error; c.error = "trailing"; return c; }
    c.op = HidOp::status;
    return c;
  default:
    c.op = HidOp::error;
    c.error = "verb";
    return c;
  }
}

}  // namespace pianousb
