// SPDX-License-Identifier: BSD-2-Clause-Patent
// A simulated WCN7860 Bluetooth controller for the host tests (bt_host.cpp drives it frame by frame, bt_bringup_host.cpp byte by
// byte through a model of the GENI UART): the vendor commands of QcaCodec.h, the firmware download (an ELF rampatch acknowledged at
// its end, an NVM acknowledged segment by segment), the speed change (its reply sent at the new speed), the address, and faults. It
// answers what the Ubuntu piano kernel's btqca/hci_qca expect of the chip (behaviour only); it is not the chip.
#pragma once
#include "HciH4.h"
#include "QcaCodec.h"
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace pianobt {
namespace sim {

inline std::vector<uint8_t> makeNvm(bool envelope, uint8_t hci0, uint8_t sleep0)
{
  std::vector<uint8_t> tags;
  auto tag = [&](uint16_t id, std::vector<uint8_t> data) {
    const uint16_t len = uint16_t(data.size());
    tags.insert(tags.end(), {uint8_t(id), uint8_t(id >> 8), uint8_t(len), uint8_t(len >> 8), 0, 0, 0, 0, 0, 0, 0, 0});
    tags.insert(tags.end(), data.begin(), data.end());
  };
  tag(1, {9, 9});
  tag(kTagBdAddr, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66});
  tag(kTagHci, {hci0, 0x00, 0x00, 0x00});
  tag(kTagDeepSleep, {sleep0});
  std::vector<uint8_t> f;
  const uint32_t tl = uint32_t(tags.size()) << 8 | 2;
  if (envelope) {
    const uint32_t outer = uint32_t(tags.size() + 4) << 8 | 4;
    f.insert(f.end(), {uint8_t(outer), uint8_t(outer >> 8), uint8_t(outer >> 16), uint8_t(outer >> 24)});
  }
  f.insert(f.end(), {uint8_t(tl), uint8_t(tl >> 8), uint8_t(tl >> 16), uint8_t(tl >> 24)});
  f.insert(f.end(), tags.begin(), tags.end());
  return f;
}

struct Sim {
  // The line.
  uint32_t hostBaud = 115200, chipBaud = 115200;
  bool flow = true;
  std::vector<std::string> log;   // what the host did, in order
  // Events queued at the speed the controller sent them; delivered only when the host listens at that speed.
  std::deque<std::pair<uint32_t, std::vector<uint8_t>>> events;
  // Files.
  std::map<std::string, std::vector<uint8_t>> files;
  // State.
  enum { kPatch, kNvm } phase = kPatch;
  std::vector<uint8_t> received[2];
  unsigned segments[2] = {0, 0};
  uint32_t expect[2] = {0, 0};
  uint8_t address[6] = {};        // least significant first
  uint8_t nvmDefault[6] = {};
  unsigned periFrames = 0, wakeInds = 0, sleeps = 0, flowOffBaud = 0, resets = 0;
  // Faults.
  bool deafAfterBaud = false, hwErrorInPatch = false, resetFails = false, badVersion = false, noBoardId = false;
  bool romIgnoresReset = false;   // no answer to HCI_Reset before the rampatch
  bool forgetsAddress = false;    // an HCI_Reset after an address write brings back the NVM's default (once)
  bool addressWritten = false;
  // The byte stream (bt_bringup_host.cpp): a framer for what arrives, the junk it could not frame.
  std::vector<uint8_t> frame;
  size_t need = 0;
  unsigned junk = 0;

  void reply(uint16_t opcode, std::vector<uint8_t> ret)
  {
    std::vector<uint8_t> e = {kEvCommandComplete, uint8_t(3 + ret.size()), 0x01, uint8_t(opcode), uint8_t(opcode >> 8)};
    e.insert(e.end(), ret.begin(), ret.end());
    events.push_back({chipBaud, e});
  }
  void vendorEvent() { events.push_back({chipBaud, {kEvVendor, 2, 0x04, 0x00}}); }

  // One whole frame from the host (an H4 command, an IBS byte or the WCN7860 peripheral baud frame), at the right speed.
  int handle(const uint8_t *b, size_t n)
  {
    if (n == 1 && b[0] == kIbsWakeInd) {
      ++wakeInds;
      return 0;
    }
    if (n == kPeriFrameBytes && b[0] == 0x31) {
      ++periFrames;
      log.push_back("peri");
      return 0;
    }
    if (n < 4 || b[0] != kH4Command || n != size_t(4) + b[3]) return -1;
    const uint16_t op = le16(b + 1);
    const uint8_t *p = b + 4;
    char line[32];
    std::snprintf(line, sizeof line, "cmd %04x", op);
    if (op != kOpEdl || p[0] != kEdlTlvSegment) log.push_back(line);
    switch (op) {
    case kOpEdl:
      switch (p[0]) {
      case kEdlVersion:
        if (badVersion) reply(op, {0x00, 0x19, 0x00});
        else reply(op, {0x00, 0x19, 0x55, 0x13, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x00, 0x02, 0x21, 0x40});
        break;
      case kEdlTlvSegment: {
        const unsigned len = p[1];
        std::vector<uint8_t> &r = received[phase];
        r.insert(r.end(), p + 2, p + 2 + len);
        ++segments[phase];
        if (phase == kPatch && hwErrorInPatch && segments[phase] == 10) {
          events.push_back({chipBaud, {kEvHardwareError, 1, 0x0b}});
          break;
        }
        const bool last = r.size() >= expect[phase] || len < kSegmentMax;
        if (phase == kNvm || last) {
          if (last && phase == kPatch) vendorEvent();
          reply(op, {0x00, 0x1e});
        }
        break;
      }
      case kEdlBoardId:
        phase = kNvm;
        if (!noBoardId) reply(op, {0x00, 0x23, 0x02, 0x00, 0x2e});
        else reply(op, {0x01});
        break;
      case kEdlBuildInfo: reply(op, {0x00, 0x20, 0x05, 'B', 'T', 'F', 'W', '1'}); break;
      }
      break;
    case kOpSetBaud: {
      static const uint32_t bauds[] = {115200, 57600, 38400, 19200, 9600, 230400, 250000, 460800, 500000, 720000,
                                       921600, 1000000, 1250000, 2000000, 3000000, 4000000, 1600000, 3200000};
      if (flow) return -1;   // the host must have turned flow control off first
      if (!deafAfterBaud) chipBaud = bauds[p[0]];
      else chipBaud = 1;
      flowOffBaud = chipBaud;
      reply(op, {0x00});     // sent at the new speed: the host may or may not hear it
      break;
    }
    case kOpLogging: reply(op, {0x00}); break;
    case kOpReset:
      ++resets;
      if (romIgnoresReset && !segments[kPatch]) break;
      if (forgetsAddress && addressWritten) {
        std::memcpy(address, nvmDefault, 6);
        forgetsAddress = false;
      }
      reply(op, {uint8_t(resetFails && segments[kPatch] ? 0x03 : 0x00)});
      break;
    case kOpReadLocalVersion: reply(op, {0x00, 0x0d, 0x00, 0x00, 0x0d, 0x1d, 0x00, 0x07, 0x00}); break;
    case kOpReadBdAddr: {
      std::vector<uint8_t> r = {0x00};
      r.insert(r.end(), address, address + 6);
      reply(op, r);
      break;
    }
    case kOpWriteBdAddr:
      for (unsigned i = 0; i < 6; ++i) address[i] = p[5 - i];
      addressWritten = true;
      reply(op, {0x00});
      break;
    }
    return 0;
  }
  // bt_host.cpp's frame-at-a-time line: a frame at another speed is garbage to the controller.
  int send(const uint8_t *b, size_t n)
  {
    if (hostBaud != chipBaud) return 0;
    return handle(b, n);
  }
  // bt_bringup_host.cpp's byte stream (already at the controller's speed).
  void rxByte(uint8_t b)
  {
    if (frame.empty()) {
      if (isIbs(b)) {
        handle(&b, 1);
        return;
      }
      if (b == 0x31) need = kPeriFrameBytes;
      else if (b == kH4Command) need = 4;
      else {
        ++junk;
        return;
      }
    }
    frame.push_back(b);
    if (frame[0] == kH4Command && frame.size() == 4) need = size_t(4) + frame[3];
    if (frame.size() < need) return;
    handle(frame.data(), frame.size());
    frame.clear();
    need = 0;
  }
  int event(uint8_t *ev, size_t cap)
  {
    while (!events.empty()) {
      auto e = events.front();
      events.pop_front();
      if (e.first != hostBaud) continue;   // sent at another speed: lost
      if (e.second.size() > cap) return -1;
      std::memcpy(ev, e.second.data(), e.second.size());
      return int(e.second.size());
    }
    return 0;   // the timeout
  }
};

inline void stock(Sim &s)
{
  std::vector<uint8_t> patch(kStockRampatchBytes);
  for (size_t i = 0; i < patch.size(); ++i) patch[i] = uint8_t(i * 7 + 3);
  const uint8_t elf[7] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
  std::memcpy(patch.data(), elf, sizeof elf);
  s.files["gngbtfw20.mbn"] = patch;
  s.files["gngbtnv20.b2e"] = makeNvm(true, 0x83, 0x00);
  s.files["gngbtnv20.bin"] = makeNvm(false, 0x03, 0x00);
  s.expect[Sim::kPatch] = kStockRampatchBytes;
  s.expect[Sim::kNvm] = uint32_t(s.files["gngbtnv20.b2e"].size());
  const uint8_t def[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};   // the NVM's default, least significant first
  std::memcpy(s.address, def, 6);
  std::memcpy(s.nvmDefault, def, 6);
}

}  // namespace sim
}  // namespace pianobt
