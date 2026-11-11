// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of HidInput.h (the input port's command parser and line framing) and of the report layouts against the descriptors the
// HID devices declare (package/PianoKeyboard/HidDescriptors.h): every report the parser builds must have the size and report ID
// that a walk of the descriptor gives.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. -I../../PianoKeyboard hid_input_host.cpp -o /tmp/hid_input_host && /tmp/hid_input_host
#include "HidInput.h"
#include "HidDescriptors.h"
#include "Wn8030Piano.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

using namespace pianousb;
static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static_assert(kKeyboardReportBytes == pianokeyboard::kKeyboardBytes && kConsumerReportBytes == pianokeyboard::kConsumerBytes &&
              kPointerReportBytes == pianokeyboard::kPointerBytes, "report sizes are PianoKeyboard's");
static_assert(kReportKeyboard == pianokeyboard::kIdKeyboard && kReportConsumer == pianokeyboard::kIdConsumer, "report IDs");

// Input report sizes (bytes, the ID byte included when the descriptor uses IDs) by report ID, from a HID 1.11 descriptor walk.
static std::map<unsigned, unsigned> inputReports(const uint8_t *d, unsigned n)
{
  std::map<unsigned, unsigned> bits;
  unsigned size = 0, count = 0, id = 0;
  bool ids = false;
  for (unsigned i = 0; i < n;) {
    const uint8_t prefix = d[i];
    const unsigned len = (prefix & 3) == 3 ? 4 : prefix & 3;
    uint32_t v = 0;
    for (unsigned b = 0; b < len; ++b) v |= uint32_t(d[i + 1 + b]) << (8 * b);
    switch (prefix & 0xfc) {
    case 0x74: size = v; break;               // Report Size
    case 0x94: count = v; break;              // Report Count
    case 0x84: id = v; ids = true; break;     // Report ID
    case 0x80: bits[id] += size * count; break;   // Input
    default: break;
    }
    i += 1 + len;
  }
  std::map<unsigned, unsigned> bytes;
  for (auto &[k, b] : bits) bytes[k] = (b + 7) / 8 + (ids ? 1 : 0);
  return bytes;
}

static HidCommand parse(const char *s) { return parseHidCommand(s); }

// hid_input_host FILE: every line of FILE must parse (tooling/native/test_piano_input.py feeds it what piano-input.py sends).
static int parseFile(const char *path)
{
  FILE *f = std::fopen(path, "r");
  if (!f) { std::printf("cannot open %s\n", path); return 2; }
  char line[512];
  unsigned n = 0, bad = 0;
  while (std::fgets(line, sizeof line, f)) {
    line[std::strcspn(line, "\r\n")] = 0;
    ++n;
    const HidCommand c = parseHidCommand(line);
    if (c.op == HidOp::error || c.op == HidOp::none) { std::printf("line %u refused: %s\n", n, c.error ? c.error : "?"); ++bad; }
  }
  std::fclose(f);
  std::printf("%u lines, %u refused\n", n, bad);
  return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
  if (argc > 1 && argv[1][0] != 'v') return parseFile(argv[1]);
  // Descriptors vs. the parser's reports.
  auto kb = inputReports(pianokeyboard::kKeyboardDescriptor, sizeof pianokeyboard::kKeyboardDescriptor);
  auto pt = inputReports(pianokeyboard::kPointerDescriptor, sizeof pianokeyboard::kPointerDescriptor);
  CHECK(kb.size() == 2 && kb[kReportKeyboard] == kKeyboardReportBytes && kb[kReportConsumer] == kConsumerReportBytes);
  CHECK(pt.size() == 1 && pt[0] == kPointerReportBytes);

  // Keyboard.
  HidCommand c = parse("K 2 4");                  // shift + a
  CHECK(c.op == HidOp::keyboard && c.bytes == 9 && c.keysDown);
  const uint8_t shiftA[9] = {5, 2, 0, 4, 0, 0, 0, 0, 0};
  CHECK(std::memcmp(c.report, shiftA, 9) == 0);
  c = parse("K 0");
  CHECK(c.op == HidOp::keyboard && !c.keysDown && c.report[0] == 5 && c.report[1] == 0 && c.report[3] == 0);
  c = parse("  K 8 2c  ");                        // cmd + space, spaces around
  CHECK(c.op == HidOp::keyboard && c.report[1] == 8 && c.report[3] == 0x2c);
  c = parse("K ff 4 5 6 7 8 9");
  CHECK(c.op == HidOp::keyboard && c.report[1] == 0xff && c.report[8] == 9);
  CHECK(parse("K 0 4 5 6 7 8 9 a").op == HidOp::error);   // seven keys
  CHECK(parse("K 100").op == HidOp::error);
  CHECK(parse("K 0 100").op == HidOp::error);
  CHECK(parse("K").op == HidOp::error);
  CHECK(parse("K 0x8").op == HidOp::error);
  CHECK(parse("Kx 0").op == HidOp::error);
  CHECK(parse("K 0 4z").op == HidOp::error);
  // Consumer.
  c = parse("C e9");
  CHECK(c.op == HidOp::consumer && c.bytes == 3 && c.report[0] == 6 && c.report[1] == 0xe9 && c.report[2] == 0 && c.keysDown);
  c = parse("C 3ff");
  CHECK(c.op == HidOp::consumer && c.report[1] == 0xff && c.report[2] == 3);
  CHECK(parse("C 0").op == HidOp::consumer && !parse("C 0").keysDown);
  CHECK(parse("C 400").op == HidOp::error && parse("C 1 2").op == HidOp::error);
  // Pointer.
  c = parse("P 0 -5 12");
  CHECK(c.op == HidOp::pointer && c.bytes == 7 && !c.keysDown);
  const uint8_t move[7] = {0, 0xfb, 0xff, 12, 0, 0, 0};
  CHECK(std::memcmp(c.report, move, 7) == 0);
  c = parse("P 1 0 0");
  CHECK(c.op == HidOp::pointer && c.report[0] == 1 && c.keysDown);
  c = parse("P 0 0 0 -3 +2");
  CHECK(c.op == HidOp::pointer && c.report[5] == 0xfd && c.report[6] == 2);
  c = parse("P 0 32767 -32767");
  CHECK(c.op == HidOp::pointer && c.report[1] == 0xff && c.report[2] == 0x7f && c.report[3] == 0x01 && c.report[4] == 0x80);
  CHECK(parse("P 0 32768 0").op == HidOp::error && parse("P 0 0 -32768").op == HidOp::error);
  CHECK(parse("P 8 0 0").op == HidOp::error && parse("P 0 0 0 128").op == HidOp::error && parse("P 0 0 0 0 -128").op == HidOp::error);
  CHECK(parse("P 0 1").op == HidOp::error && parse("P 0 1 2 3 4 5").op == HidOp::error && parse("P 0 1.5 2").op == HidOp::error);
  CHECK(parse("P 0 9999999999 0").op == HidOp::error);
  // Others.
  CHECK(parse("R").op == HidOp::release && parse(" R ").op == HidOp::release && parse("R 1").op == HidOp::error);
  CHECK(parse("?").op == HidOp::status && parse("? x").op == HidOp::error);
  CHECK(parse("").op == HidOp::error && parse("Z 1").op == HidOp::error && parse("k 0").op == HidOp::error);
  for (const char *good : {"K 0 4", "P 0 1 1", "C 1"}) CHECK(parse(good).error == nullptr);

  // Framing: CR, LF and CRLF end lines, empty lines vanish, an overlong line is dropped whole and the next one is intact.
  LineAssembler a;
  std::string got;
  int dropped = 0;
  auto feed = [&](const std::string &bytes) {
    for (char ch : bytes) {
      const int r = a.push(ch);
      if (r > 0) got += std::string(a.line()) + "|";
      if (r < 0) ++dropped;
    }
  };
  feed("K 0 4\r\nK 0\n\n\rR\r");
  CHECK(got == "K 0 4|K 0|R|" && dropped == 0);
  got.clear();
  feed(std::string(300, 'x') + "\nK 0\n");
  CHECK(got == "K 0|" && dropped == 1);
  got.clear();
  feed(std::string(kLineMax - 2, 'y') + "\n");
  CHECK(got.size() == kLineMax - 2 + 1);

  std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "ok", failures);
  return failures ? 1 : 0;
}
