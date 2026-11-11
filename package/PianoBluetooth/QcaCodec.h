// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Qualcomm vendor side of HCI for the WCN7860 ("GANGES", ROM 0x20 on piano): the vendor commands and their replies, the firmware
// file names, the NVM file's tags, the download plan, and the controller set-up sequence (level 4 of docs/drivers/bluetooth.md) as one
// blocking function over callbacks, so the host tests run it against a simulated controller and can make any step fail. Portable:
// no allocation (the caller lends the NVM buffer), no formatting library, nothing logged that identifies a unit (the Bluetooth
// address is compared and written, never reported).
//
// The behaviour is that of the Ubuntu piano kernel's drivers/bluetooth/btqca.c, btqca.h and hci_qca.c (GPL-2.0; the WCN7860 support
// is its commit 490bebf5dd8f), read as the specification of the wire protocol only: opcodes, sub-commands, reply layouts, file
// names, tag numbers and the order of the steps are facts; no code, comment or table is copied. Where this file differs from Linux
// on purpose, the comment says so.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "HciH4.h"

namespace pianobt {

// ------------------------------------------------------------------------------------------------------------------- opcodes
constexpr uint16_t kOpEdl = 0xfc00;            // the "EDL" vendor command: one sub-command byte, then its arguments
constexpr uint16_t kOpNvmAccess = 0xfc0b, kOpPreShutdown = 0xfc08, kOpWriteBdAddr = 0xfc14, kOpLogging = 0xfc17,
                   kOpSetBaud = 0xfc48;
constexpr uint16_t kOpReset = 0x0c03, kOpReadLocalVersion = 0x1001, kOpReadBdAddr = 0x1009;
constexpr uint8_t kEdlVersion = 0x19, kEdlTlvSegment = 0x1e, kEdlBuildInfo = 0x20, kEdlBoardId = 0x23, kEdlPatchConfig = 0x28;
constexpr uint8_t kLoggingSubOp = 0x14;
constexpr unsigned kSegmentMax = 243;          // bytes of a firmware file per EDL segment command
// The baud codes of the vendor baud command (btqca.h's qca_baudrate). Linux refuses codes above 3.2 Mbit/s's.
struct BaudCode {
  uint32_t baud;
  uint8_t code;
};
constexpr BaudCode kBaudCodes[] = {{115200, 0x00},  {57600, 0x01},   {38400, 0x02},   {19200, 0x03},   {9600, 0x04},
                                   {230400, 0x05},  {250000, 0x06},  {460800, 0x07},  {500000, 0x08},  {720000, 0x09},
                                   {921600, 0x0a},  {1000000, 0x0b}, {1250000, 0x0c}, {2000000, 0x0d}, {3000000, 0x0e},
                                   {4000000, 0x0f}, {1600000, 0x10}, {3200000, 0x11}, {3500000, 0x12}};
constexpr uint8_t kBaudCodeMax = 0x11;
inline int baudCode(uint32_t baud)
{
  for (const BaudCode &b : kBaudCodes)
    if (b.baud == baud) return b.code;
  return -1;
}

// ------------------------------------------------------------------------------------------------------------- commands (H4)
inline size_t cmdEdl(uint8_t *out, size_t cap, uint8_t sub) { return h4Command(out, cap, kOpEdl, &sub, 1); }
inline size_t cmdSegment(uint8_t *out, size_t cap, const uint8_t *data, unsigned len)
{
  if (!len || len > kSegmentMax || cap < 6 + len) return 0;
  uint8_t params[2 + kSegmentMax];
  params[0] = kEdlTlvSegment;
  params[1] = uint8_t(len);
  for (unsigned i = 0; i < len; ++i) params[2 + i] = data[i];
  return h4Command(out, cap, kOpEdl, params, uint8_t(2 + len));
}
inline size_t cmdSetBaud(uint8_t *out, size_t cap, uint8_t code)
{
  return code <= kBaudCodeMax ? h4Command(out, cap, kOpSetBaud, &code, 1) : 0;
}
// WCN7860 only (hci_qca.c peri_set_baudrate in Ubuntu's kernel): before the vendor baud command, an IBS WAKE_IND, 10 ms, then these
// seven raw bytes (no H4 indicator of its own: the first byte is 0x31) carrying the same baud code, then 100 ms.
constexpr size_t kPeriFrameBytes = 7;
inline size_t periBaudFrame(uint8_t *out, size_t cap, uint8_t code)
{
  if (cap < kPeriFrameBytes || code > kBaudCodeMax) return 0;
  const uint8_t f[kPeriFrameBytes] = {0x31, 0x00, 0xf1, 0xff, 0x02, 0x02, code};
  for (size_t i = 0; i < kPeriFrameBytes; ++i) out[i] = f[i];
  return kPeriFrameBytes;
}
inline size_t cmdDisableLogging(uint8_t *out, size_t cap)
{
  const uint8_t p[2] = {kLoggingSubOp, 0x00};
  return h4Command(out, cap, kOpLogging, p, 2);
}
inline size_t cmdPlain(uint8_t *out, size_t cap, uint16_t opcode) { return h4Command(out, cap, opcode, nullptr, 0); }
// The public address, most significant byte first (the order it is written in, "XX:XX:..."): the controller wants it so.
inline size_t cmdWriteBdAddr(uint8_t *out, size_t cap, const uint8_t display[6]) { return h4Command(out, cap, kOpWriteBdAddr, display, 6); }

// ---------------------------------------------------------------------------------------------------------------- replies
// From WCN3991 on (and so on WCN7860) the EDL replies are Command Complete events of 0xfc00 whose return parameters start with
// 00 (EDL_CMD_REQ_RES_EVT) and the sub-command.
inline bool edlReply(const CommandComplete &cc, uint8_t sub) { return cc.opcode == kOpEdl && cc.retLen >= 2 && cc.ret[0] == 0 && cc.ret[1] == sub; }

struct QcaVersion {
  uint32_t product = 0, soc = 0;
  uint16_t patch = 0, rom = 0;
  uint32_t controller = 0;   // btqca's "controller version": the SoC id's low half above the ROM version
  uint8_t romCode = 0;       // what the file names carry: 0x20 for 0x...0200
};
// Version: 15 return bytes: 00 19, one byte, then product (4), patch (2), ROM (2), SoC (4), little endian.
inline bool parseVersion(const CommandComplete &cc, QcaVersion &v)
{
  if (!edlReply(cc, kEdlVersion) || cc.retLen != 15) return false;
  const uint8_t *p = cc.ret + 3;
  v.product = le32(p);
  v.patch = le16(p + 4);
  v.rom = le16(p + 6);
  v.soc = le32(p + 8);
  v.controller = (v.soc << 16) | v.rom;
  v.romCode = uint8_t(((v.controller & 0xf00u) >> 4) | (v.controller & 0xfu));
  return v.soc != 0 && v.rom != 0;
}
// Board id: 00 23, then three bytes of which the last two are the id (high byte first).
inline bool parseBoardId(const CommandComplete &cc, uint16_t &bid)
{
  if (!edlReply(cc, kEdlBoardId) || cc.retLen < 5) return false;
  bid = uint16_t(cc.ret[3] << 8 | cc.ret[4]);
  return true;
}
// Build information: 00 20, a length, that many characters (copied, non-printable ones as '?').
inline bool parseBuildInfo(const CommandComplete &cc, char *out, size_t cap)
{
  if (!edlReply(cc, kEdlBuildInfo) || cc.retLen < 3 || cc.retLen < size_t(3) + cc.ret[2] || !cap) return false;
  size_t n = cc.ret[2] < cap - 1 ? cc.ret[2] : cap - 1;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t c = cc.ret[3 + i];
    out[i] = c >= 0x20 && c < 0x7f ? char(c) : '?';
  }
  out[n] = 0;
  return true;
}
// A firmware segment's acknowledgement: 00 1e and nothing else.
inline bool segmentAck(const CommandComplete &cc) { return edlReply(cc, kEdlTlvSegment) && cc.retLen == 2; }
struct LocalVersion {
  uint8_t hci = 0, lmp = 0;
  uint16_t hciRevision = 0, manufacturer = 0, lmpSubversion = 0;
};
inline bool parseLocalVersion(const CommandComplete &cc, LocalVersion &l)
{
  if (cc.opcode != kOpReadLocalVersion || cc.retLen != 9 || cc.ret[0] != 0) return false;
  l.hci = cc.ret[1];
  l.hciRevision = le16(cc.ret + 2);
  l.lmp = cc.ret[4];
  l.manufacturer = le16(cc.ret + 5);
  l.lmpSubversion = le16(cc.ret + 7);
  return true;
}
constexpr uint16_t kManufacturerQualcomm = 0x001d;

// ------------------------------------------------------------------------------------------------------------- file names
// No formatting library here: "gngbtfw" + two hex digits + ".mbn"; "gngbtnv" + ROM + ".b" + board id (at least two hex digits), or
// ".bin" when the board id is 0 or 0xffff (btqca's qca_get_nvm_name_by_board for this part, the "qca/" directory dropped).
namespace detail {
inline bool put(char *out, size_t cap, size_t &at, const char *s)
{
  for (; *s; ++s) {
    if (at + 1 >= cap) return false;
    out[at++] = *s;
  }
  out[at] = 0;
  return true;
}
inline bool hex(char *out, size_t cap, size_t &at, uint32_t v, unsigned minDigits)
{
  char d[8];
  unsigned n = 0;
  do {
    d[n++] = "0123456789abcdef"[v & 15u];
    v >>= 4;
  } while (v && n < 8);
  while (n < minDigits && n < 8) d[n++] = '0';
  while (n) {
    if (at + 1 >= cap) return false;
    out[at++] = d[--n];
  }
  out[at] = 0;
  return true;
}
}  // namespace detail
inline bool rampatchName(char *out, size_t cap, uint8_t rom)
{
  size_t at = 0;
  if (!cap) return false;
  out[0] = 0;
  return detail::put(out, cap, at, "gngbtfw") && detail::hex(out, cap, at, rom, 2) && detail::put(out, cap, at, ".mbn");
}
inline bool nvmName(char *out, size_t cap, uint8_t rom, uint16_t bid)
{
  size_t at = 0;
  if (!cap) return false;
  out[0] = 0;
  if (!detail::put(out, cap, at, "gngbtnv") || !detail::hex(out, cap, at, rom, 2)) return false;
  if (bid == 0 || bid == 0xffff) return detail::put(out, cap, at, ".bin");
  return detail::put(out, cap, at, ".b") && detail::hex(out, cap, at, bid, 2);
}

// ------------------------------------------------------------------------------------------------------- firmware files
// The rampatch is an ELF (".mbn", WCN7860) or a TLV patch; the NVM is a TLV file of tags. Download modes (btqca.h): 0 every segment
// is acknowledged, 1 no vendor event, 2 no Command Complete, 3 neither. An ELF is sent in mode 3 (only the last segment, or a short
// one, is acknowledged), a TLV patch in the mode its header names, the NVM in mode 0.
enum class FwKind : uint8_t { unknown, elf, tlvPatch, tlvNvm };
struct DownloadPlan {
  FwKind kind = FwKind::unknown;
  uint32_t bytes = 0;
  unsigned segments = 0;
  uint8_t mode = 0;
  const char *why = nullptr;
};
inline bool isElf(const uint8_t *p, size_t n) { return n >= 7 && p[0] == 0x7f && p[1] == 'E' && p[2] == 'L' && p[3] == 'F'; }
// `head` is the first bytes of the file (at least 16, or the whole file when shorter).
inline DownloadPlan planDownload(const uint8_t *head, size_t headBytes, uint32_t fileBytes, bool nvm)
{
  DownloadPlan p;
  p.bytes = fileBytes;
  p.segments = (fileBytes + kSegmentMax - 1) / kSegmentMax;
  if (!fileBytes) {
    p.why = "empty file";
    return p;
  }
  if (nvm) {
    if (fileBytes < 4) {
      p.why = "an NVM file shorter than its header";
      return p;
    }
    p.kind = FwKind::tlvNvm;
    p.mode = 0;
  } else if (isElf(head, headBytes < fileBytes ? headBytes : fileBytes)) {
    p.kind = FwKind::elf;
    p.mode = 3;
  } else {
    // A TLV patch: a 4-byte type/length word, then total size (4), data length (4), format, signature, download mode, ...
    if (fileBytes < 4 + 24 || headBytes < 15) {
      p.why = "a TLV patch shorter than its header";
      return p;
    }
    p.kind = FwKind::tlvPatch;
    p.mode = head[14];
  }
  return p;
}
// Whether segment `index` (of `bytes` bytes) waits for its acknowledgement: always the last and a short one; the others unless the
// mode skips the vendor event (1, 3).
inline bool segmentWaits(const DownloadPlan &p, unsigned index, unsigned bytes)
{
  if (index + 1 >= p.segments || bytes < kSegmentMax) return true;
  return p.mode != 1 && p.mode != 3;
}

// The NVM file: a type/length word (type in 7:0, length in 31:8); type 4 encloses a second header; type 2 holds the tags, each a
// 12-byte header (id, length, eight reserved bytes) and its data. Tags of interest: 2 the default address, 17 the HCI transport
// (data[0] bit 7 software in-band sleep, data[1] the baud code from WCN3991 on), 27 the sleep enables (data[0] bit 0 deep sleep).
constexpr uint16_t kTagBdAddr = 2, kTagHci = 17, kTagDeepSleep = 27;
struct NvmInfo {
  bool ok = false;
  const char *why = nullptr;
  unsigned tags = 0;
  bool hci = false, sleep = false, address = false;
  uint8_t hciBefore = 0, hciAfter = 0, baudBefore = 0, baudAfter = 0, sleepBefore = 0, sleepAfter = 0;
  size_t addressAt = 0;   // offset of the default address's six bytes in the file (little endian, as Read BD_ADDR returns it)
};
// Patches the tags in place. ibs / deepSleep: 1 set the bit (what Linux always does), 0 clear it, -1 leave the file's value. The
// baud code is the speed the host will run at (Linux writes it unconditionally). Bounds as btqca's qca_tlv_check_data.
inline NvmInfo patchNvm(uint8_t *f, size_t n, uint8_t baud, int ibs, int deepSleep)
{
  NvmInfo r;
  if (n < 4) {
    r.why = "shorter than its header";
    return r;
  }
  size_t at = 0;
  uint32_t typeLen = le32(f);
  if ((typeLen & 0xffu) == 4) {
    if (n < 8) {
      r.why = "an enclosing header without a second one";
      return r;
    }
    at = 4;
    typeLen = le32(f + 4);
  }
  if ((typeLen & 0xffu) != 2) {
    r.ok = true;   // not a tag list: sent as it is (Linux does the same)
    r.why = "not a tag list (type is not 2)";
    return r;
  }
  const size_t length = typeLen >> 8, data = at + 4;
  if (n < data + length) {
    r.why = "the tag list is longer than the file";
    return r;
  }
  for (size_t idx = 0; idx + 12 <= length;) {
    uint8_t *t = f + data + idx;
    const uint16_t id = le16(t), tagLen = le16(t + 2);
    if (length < idx + 12 + tagLen) {
      r.why = "a tag runs past the list";
      return r;
    }
    uint8_t *d = t + 12;
    ++r.tags;
    if (id == kTagBdAddr) {
      if (tagLen != 6) {
        r.why = "the address tag is not six bytes";
        return r;
      }
      r.address = true;
      r.addressAt = size_t(d - f);
    } else if (id == kTagHci) {
      if (tagLen < 3) {
        r.why = "the HCI tag is shorter than three bytes";
        return r;
      }
      r.hci = true;
      r.hciBefore = d[0];
      r.baudBefore = d[1];
      if (ibs == 1) d[0] = uint8_t(d[0] | 0x80u);
      else if (ibs == 0) d[0] = uint8_t(d[0] & ~0x80u);
      d[1] = baud;
      r.hciAfter = d[0];
      r.baudAfter = d[1];
    } else if (id == kTagDeepSleep) {
      if (tagLen < 1) {
        r.why = "the sleep tag is empty";
        return r;
      }
      r.sleep = true;
      r.sleepBefore = d[0];
      if (deepSleep == 1) d[0] = uint8_t(d[0] | 0x01u);
      else if (deepSleep == 0) d[0] = uint8_t(d[0] & ~0x01u);
      r.sleepAfter = d[0];
    }
    idx += 12 + tagLen;
  }
  r.ok = true;
  return r;
}

// ------------------------------------------------------------------------------------------------------ the set-up sequence
// The order of btqca's qca_uart_setup with hci_qca's WCN7860 steps around it: version at 115200; the peripheral baud frame; flow
// control off (RFR deasserted), the vendor baud command, the line drained, 10 ms, the host to the new speed, flow control on; the
// rampatch; 10 ms; the board id; the NVM (patched); SoC logging off; HCI_Reset; the build information; then (ours) the local
// version, the address check and the address. Two deliberate differences from Linux: the version is read again at the new speed
// before the download (Linux does not check that the speed change took), and a board id read that fails ends the set-up only when
// the default NVM is missing too (Linux ignores that error entirely and falls back to .bin, as this does).
struct SetupIo {
  void *ctx = nullptr;
  int (*send)(void *ctx, const uint8_t *bytes, size_t n) = nullptr;            // into the engine, whole; 0 or negative
  int (*drain)(void *ctx, unsigned timeoutMs) = nullptr;                       // every byte sent has left the line; 0 or negative
  int (*event)(void *ctx, uint8_t *ev, size_t cap, unsigned timeoutMs) = nullptr;   // the next HCI event: its length, 0 timeout, <0 fault
  int (*hostBaud)(void *ctx, uint32_t baud) = nullptr;
  int (*hostFlow)(void *ctx, bool on) = nullptr;                               // on: CTS honoured, RFR automatic; off: RFR deasserted
  void (*sleepMs)(void *ctx, unsigned ms) = nullptr;
  int (*fwSize)(void *ctx, const char *name, uint32_t *bytes) = nullptr;       // 0, or negative when the file is not there
  int (*fwRead)(void *ctx, const char *name, uint32_t offset, uint8_t *to, uint32_t n) = nullptr;
  void (*step)(void *ctx, int step, const char *what) = nullptr;
};
struct SetupOptions {
  uint32_t operBaud = 3200000;          // 0 or 115200: stay at the initial speed
  bool peri = true;                     // the WCN7860 peripheral baud frame
  bool verifyBaud = true;               // read the version again at the new speed
  int ibs = 0, deepSleep = 0;           // NVM bits: 1 set (Linux), 0 clear (the first bring-up: no in-band sleep to serve), -1 keep
  const uint8_t *address = nullptr;     // the unit's public address, display order, from a private source; never logged
  unsigned commandMs = 3000, finalSegmentMs = 10000;
  int stopAfter = 0;                    // return after this step (debugging)
};
enum SetupStep : int {
  kStepVersion = 401, kStepPeriBaud = 402, kStepBaud = 403, kStepRampatch = 404, kStepBoardId = 405, kStepNvm = 406,
  kStepLogging = 407, kStepReset = 408, kStepBuildInfo = 409, kStepLocalVersion = 410, kStepAddress = 411,
};
enum SetupError : int {
  kSetupOk = 0, kSetupStopped = -1, kSetupArgument = -2, kSetupTransport = -3, kSetupTimeout = -4, kSetupBadReply = -5,
  kSetupStatus = -6, kSetupHardwareError = -7, kSetupNoFile = -8, kSetupBadFile = -9, kSetupBaud = -10,
};
inline const char *setupErrorName(int e)
{
  switch (e) {
  case kSetupOk: return "none";
  case kSetupStopped: return "stopped here on request";
  case kSetupArgument: return "bad arguments";
  case kSetupTransport: return "the UART failed";
  case kSetupTimeout: return "no reply within the timeout";
  case kSetupBadReply: return "a reply of the wrong shape";
  case kSetupStatus: return "a command failed (status)";
  case kSetupHardwareError: return "the controller reported a hardware error";
  case kSetupNoFile: return "a firmware file is missing";
  case kSetupBadFile: return "a firmware file is not usable";
  case kSetupBaud: return "the controller does not answer at the new speed";
  default: return "?";
  }
}
struct SetupResult {
  int step = 0, error = 0;
  QcaVersion version, versionAtSpeed;
  uint32_t baud = 115200;
  uint16_t boardId = 0;
  bool boardIdRead = false, nvmFallback = false;
  char rampatch[24] = {}, nvm[24] = {}, build[64] = {};
  DownloadPlan rampatchPlan, nvmPlan;
  unsigned acks = 0, unmatched = 0;
  NvmInfo nvmInfo;
  LocalVersion local;
  bool addressRead = false, addressIsNvmDefault = false, addressSet = false;
  uint16_t lastOpcode = 0;
  uint8_t lastStatus = 0, hardwareError = 0;
};

namespace detail {
struct Setup {
  const SetupIo &io;
  const SetupOptions &o;
  SetupResult &r;
  uint8_t ev[260];
  uint8_t frame[4 + 255];

  int sendFrame(size_t n)
  {
    if (!n) return kSetupArgument;
    return io.send(io.ctx, frame, n) ? kSetupTransport : kSetupOk;
  }
  // Waits for the Command Complete of `opcode`; other events are counted and passed over (the vendor event after the last
  // segment, a late Command Complete of the baud command), a hardware error or a failing Command Status ends the wait.
  int waitFor(uint16_t opcode, CommandComplete &cc, unsigned timeoutMs)
  {
    r.lastOpcode = opcode;
    for (unsigned tries = 0; tries < 64; ++tries) {
      const int n = io.event(io.ctx, ev, sizeof ev, timeoutMs);
      if (n < 0) return kSetupTransport;
      if (n == 0) return kSetupTimeout;
      if (ev[0] == kEvHardwareError && n >= 3) {
        r.hardwareError = ev[2];
        return kSetupHardwareError;
      }
      CommandStatus cs;
      if (parseCommandStatus(ev, size_t(n), cs) && cs.opcode == opcode && cs.status) {
        r.lastStatus = cs.status;
        return kSetupStatus;
      }
      if (parseCommandComplete(ev, size_t(n), cc) && cc.opcode == opcode) return kSetupOk;
      ++r.unmatched;
    }
    return kSetupTimeout;
  }
  int command(size_t n, uint16_t opcode, CommandComplete &cc, unsigned timeoutMs)
  {
    const int e = sendFrame(n);
    return e ? e : waitFor(opcode, cc, timeoutMs);
  }
  void announce(int step, const char *what)
  {
    r.step = step;
    if (io.step) io.step(io.ctx, step, what);
  }
  bool stopHere(int step) const { return o.stopAfter == step; }
  int fail(int step, int error)
  {
    r.step = -step;
    r.error = error;
    return error;
  }
  int download(const char *name, uint32_t bytes, const DownloadPlan &plan, uint8_t *buffer)
  {
    uint8_t seg[kSegmentMax];
    for (unsigned i = 0; i < plan.segments; ++i) {
      const uint32_t off = i * kSegmentMax, len = bytes - off < kSegmentMax ? bytes - off : kSegmentMax;
      const uint8_t *src = seg;
      if (buffer) src = buffer + off;
      else if (io.fwRead(io.ctx, name, off, seg, len)) return kSetupBadFile;
      int e = sendFrame(cmdSegment(frame, sizeof frame, src, len));
      if (e) return e;
      if (!segmentWaits(plan, i, len)) continue;
      CommandComplete cc;
      e = waitFor(kOpEdl, cc, i + 1 == plan.segments ? o.finalSegmentMs : o.commandMs);
      if (e) return e;
      if (!segmentAck(cc)) {
        r.lastStatus = cc.retLen ? cc.ret[0] : 0xff;
        return kSetupBadReply;
      }
      ++r.acks;
    }
    return kSetupOk;
  }
};
}  // namespace detail

inline int qcaSetup(const SetupIo &io, const SetupOptions &o, SetupResult &r, uint8_t *nvmBuffer, size_t nvmCap)
{
  if (!io.send || !io.drain || !io.event || !io.hostBaud || !io.hostFlow || !io.sleepMs || !io.fwSize || !io.fwRead || !nvmBuffer)
    return r.error = kSetupArgument;
  detail::Setup s{io, o, r, {}, {}};
  CommandComplete cc;
  int e;

  s.announce(kStepVersion, "the controller's version at 115200");
  e = s.command(cmdEdl(s.frame, sizeof s.frame, kEdlVersion), kOpEdl, cc, o.commandMs);
  if (e) return s.fail(kStepVersion, e);
  if (!parseVersion(cc, r.version)) return s.fail(kStepVersion, kSetupBadReply);
  if (s.stopHere(kStepVersion)) return r.error = kSetupStopped;

  const uint32_t target = o.operBaud ? o.operBaud : 115200u;
  const int code = baudCode(target);
  if (code < 0 || code > kBaudCodeMax) return s.fail(kStepBaud, kSetupArgument);
  if (target != 115200u) {
    if (o.peri) {
      s.announce(kStepPeriBaud, "WCN7860: in-band wake, then the peripheral baud frame");
      const uint8_t wake = kIbsWakeInd;
      if (io.send(io.ctx, &wake, 1)) return s.fail(kStepPeriBaud, kSetupTransport);
      io.sleepMs(io.ctx, 10);
      if (s.sendFrame(periBaudFrame(s.frame, sizeof s.frame, uint8_t(code)))) return s.fail(kStepPeriBaud, kSetupTransport);
      io.sleepMs(io.ctx, 100);
      if (s.stopHere(kStepPeriBaud)) return r.error = kSetupStopped;
    }
    s.announce(kStepBaud, "flow control off, the vendor baud command, the host to the new speed, flow control on");
    if (io.hostFlow(io.ctx, false)) return s.fail(kStepBaud, kSetupTransport);
    if (s.sendFrame(cmdSetBaud(s.frame, sizeof s.frame, uint8_t(code))) || io.drain(io.ctx, 100)) {
      io.hostFlow(io.ctx, true);
      return s.fail(kStepBaud, kSetupTransport);
    }
    io.sleepMs(io.ctx, 10);
    e = io.hostBaud(io.ctx, target) ? kSetupTransport : kSetupOk;
    if (io.hostFlow(io.ctx, true) && !e) e = kSetupTransport;
    if (e) return s.fail(kStepBaud, e);
    r.baud = target;
    if (o.verifyBaud) {
      e = s.command(cmdEdl(s.frame, sizeof s.frame, kEdlVersion), kOpEdl, cc, o.commandMs);
      if (e == kSetupTimeout) return s.fail(kStepBaud, kSetupBaud);
      if (e) return s.fail(kStepBaud, e);
      if (!parseVersion(cc, r.versionAtSpeed)) return s.fail(kStepBaud, kSetupBadReply);
    }
    if (s.stopHere(kStepBaud)) return r.error = kSetupStopped;
  }

  s.announce(kStepRampatch, "the rampatch, read from the firmware partition");
  if (!rampatchName(r.rampatch, sizeof r.rampatch, r.version.romCode)) return s.fail(kStepRampatch, kSetupArgument);
  uint32_t bytes = 0;
  if (io.fwSize(io.ctx, r.rampatch, &bytes)) return s.fail(kStepRampatch, kSetupNoFile);
  {
    uint8_t head[16] = {};
    const uint32_t headBytes = bytes < sizeof head ? bytes : uint32_t(sizeof head);
    if (headBytes && io.fwRead(io.ctx, r.rampatch, 0, head, headBytes)) return s.fail(kStepRampatch, kSetupBadFile);
    r.rampatchPlan = planDownload(head, headBytes, bytes, false);
  }
  if (r.rampatchPlan.kind == FwKind::unknown) return s.fail(kStepRampatch, kSetupBadFile);
  e = s.download(r.rampatch, bytes, r.rampatchPlan, nullptr);
  if (e) return s.fail(kStepRampatch, e);
  if (s.stopHere(kStepRampatch)) return r.error = kSetupStopped;
  io.sleepMs(io.ctx, 10);

  s.announce(kStepBoardId, "the board id");
  e = s.command(cmdEdl(s.frame, sizeof s.frame, kEdlBoardId), kOpEdl, cc, o.commandMs);
  r.boardIdRead = !e && parseBoardId(cc, r.boardId);
  if (!r.boardIdRead) r.boardId = 0;
  if (s.stopHere(kStepBoardId)) return r.error = kSetupStopped;

  s.announce(kStepNvm, "the NVM, read from the firmware partition and patched");
  if (!nvmName(r.nvm, sizeof r.nvm, r.version.romCode, r.boardId)) return s.fail(kStepNvm, kSetupArgument);
  if (io.fwSize(io.ctx, r.nvm, &bytes)) {
    if (!nvmName(r.nvm, sizeof r.nvm, r.version.romCode, 0)) return s.fail(kStepNvm, kSetupArgument);
    r.nvmFallback = true;
    if (io.fwSize(io.ctx, r.nvm, &bytes)) return s.fail(kStepNvm, kSetupNoFile);
  }
  if (bytes > nvmCap || !bytes) return s.fail(kStepNvm, kSetupBadFile);
  if (io.fwRead(io.ctx, r.nvm, 0, nvmBuffer, bytes)) return s.fail(kStepNvm, kSetupBadFile);
  r.nvmInfo = patchNvm(nvmBuffer, bytes, uint8_t(code), o.ibs, o.deepSleep);
  if (!r.nvmInfo.ok) return s.fail(kStepNvm, kSetupBadFile);
  r.nvmPlan = planDownload(nvmBuffer, bytes, bytes, true);
  e = s.download(r.nvm, bytes, r.nvmPlan, nvmBuffer);
  if (e) return s.fail(kStepNvm, e);
  if (s.stopHere(kStepNvm)) return r.error = kSetupStopped;

  s.announce(kStepLogging, "SoC logging off");
  e = s.command(cmdDisableLogging(s.frame, sizeof s.frame), kOpLogging, cc, o.commandMs);
  if (e) return s.fail(kStepLogging, e);
  r.lastStatus = cc.retLen ? cc.ret[0] : 0;

  s.announce(kStepReset, "HCI_Reset");
  e = s.command(cmdPlain(s.frame, sizeof s.frame, kOpReset), kOpReset, cc, o.commandMs);
  if (e) return s.fail(kStepReset, e);
  if (!cc.retLen || cc.ret[0]) {
    r.lastStatus = cc.retLen ? cc.ret[0] : 0xff;
    return s.fail(kStepReset, kSetupStatus);
  }
  if (s.stopHere(kStepReset)) return r.error = kSetupStopped;

  s.announce(kStepBuildInfo, "the firmware's build information");
  e = s.command(cmdEdl(s.frame, sizeof s.frame, kEdlBuildInfo), kOpEdl, cc, o.commandMs);
  if (e) return s.fail(kStepBuildInfo, e);
  if (!parseBuildInfo(cc, r.build, sizeof r.build)) return s.fail(kStepBuildInfo, kSetupBadReply);

  s.announce(kStepLocalVersion, "Read Local Version Information");
  e = s.command(cmdPlain(s.frame, sizeof s.frame, kOpReadLocalVersion), kOpReadLocalVersion, cc, o.commandMs);
  if (e) return s.fail(kStepLocalVersion, e);
  if (!parseLocalVersion(cc, r.local)) return s.fail(kStepLocalVersion, kSetupBadReply);

  s.announce(kStepAddress, "the address: the NVM's default or not, and the unit's own when one was given (never logged)");
  e = s.command(cmdPlain(s.frame, sizeof s.frame, kOpReadBdAddr), kOpReadBdAddr, cc, o.commandMs);
  if (e) return s.fail(kStepAddress, e);
  if (cc.retLen != 7 || cc.ret[0]) return s.fail(kStepAddress, kSetupBadReply);
  r.addressRead = true;
  if (r.nvmInfo.address) {
    bool same = true;
    for (unsigned i = 0; i < 6; ++i) same = same && cc.ret[1 + i] == nvmBuffer[r.nvmInfo.addressAt + i];
    r.addressIsNvmDefault = same;
  }
  if (o.address) {
    e = s.command(cmdWriteBdAddr(s.frame, sizeof s.frame, o.address), kOpWriteBdAddr, cc, o.commandMs);
    if (e) return s.fail(kStepAddress, e);
    e = s.command(cmdPlain(s.frame, sizeof s.frame, kOpReadBdAddr), kOpReadBdAddr, cc, o.commandMs);
    if (e) return s.fail(kStepAddress, e);
    if (cc.retLen != 7 || cc.ret[0]) return s.fail(kStepAddress, kSetupBadReply);
    bool same = true;
    for (unsigned i = 0; i < 6; ++i) same = same && cc.ret[1 + i] == o.address[5 - i];   // the reply is least significant first
    r.addressSet = same;
    if (!same) return s.fail(kStepAddress, kSetupBadReply);
  }
  r.step = kStepAddress;
  r.error = kSetupOk;
  return kSetupOk;
}

}  // namespace pianobt
