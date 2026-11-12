// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Novatek NT36532E touch controller as piano (Xiaomi Pad 8 Pro) wires it, over an abstract chip-select-framed SPI transfer:
// the host-download (THP) protocol only, no I/O of its own, so tests/touch_host.cpp runs all of it against a chip model.
//
// The controller is a TDDI (it also drives the panel) with no flash for its touch firmware: the host resets its touch MCU, finds
// its trim ID, writes the firmware image into its SRAM with the hardware CRC check, and the MCU then streams raw electrode frames
// (40 x 60 int16 matrices) behind a rising edge of GPIO162; it reports no coordinates. Facts and their sources (UPSTREAM):
//   framing: write {0x80 | offset, data...}; page select {0xff, addr >> 15, addr >> 7} (= the write of offset 0x7f), then the
//     offset is addr & 0x7f and the address auto-increments; read {offset, dummy, zeros...} with the data from the third byte
//   identify: engine reset 0x7fff80 <- 0x5a; per hw-register table {trim, swrst} (3 tables, 5 tries each) bootloader reset
//     swrst <- 0x69, 6 zero bytes written at the trim address, 6 bytes read back: NT36532 when bytes 3..5 are 32 65 03 (row 0:
//     byte 0 = 0x0e, the "E"); ENB_CASC 0x1fb12c bit 0 clear = cascade; NT36532E cascade: event buffer 0x11c400, poll info 0x1093d8
//   download: bootloader reset; the ILM/DLM bank (SRAM address, size, golden CRC: 0x1fb528/0x1fb518/0x1fb500, 0x1fb52c/0x1fb530/
//     0x1fb504); auto copy 0x1fc925 <- 0x56 when the image has a cascaded second header (polled back to 0); every partition, size + 1
//     bytes; 6 zero bytes at event + 0x60, {0xae, 0} at event + 0x50 (firmware CRC on); BOOT_RDY 0x1fb50d <- 1; reset state
//     (event + 0x60) 0xa0..0xaf within 10 x 10 ms; three attempts
//   start: payload length from poll info bytes 2..3; firmware info at event + 0x78 (version, its complement, x and y counts);
//     calibrated when the reset state reaches 0xa3; host commands {0xbf, cmd, 0, value LE16} at event + 0x50, taken when the
//     first byte reads 0, in the order and with the values piano's stock driver and its Ubuntu send (0x04 = 0: stylus scanning
//     off, 0x19 = 1: the idle baseline updated, 0x01 = 2: operating mode 2, which the stock driver calls idle; 1 is active)
//   frame: buffer[0] = 0, buffer[1..] = event buffer bytes 0..; a 257-byte transport block, then the THP packet: +0 marker,
//     +2 counter LE16, +4 checksum LE16 (+12 its complement), +8 covered length in words LE32 (+16 its complement), +24 scan mode,
//     +28 frame number LE16, +48 columns, +49 rows, +56 data type, +64 the rows x columns int16 LE matrix (row-major); the 16-bit
//     words from +20 sum to minus the checksum. A transport block of 0x77 is a handshake (no frame); of 0xfd/0xfe the watchdog.
// Image layout (the vendor's host-download .bin): the last 4 KiB sector ending in "NVT" or "MOD" bounds it, its first byte the
// version (the next one its complement); the ILM and DLM records at 0 and 12 {bin, sram, size}; info and overlay records of 16
// bytes {sram, size, bin, crc}. Nothing here writes the controller's flash (it has none for the touch firmware) or sends a panel
// command.
#ifndef PIANO_TOUCH_NT36532_H
#define PIANO_TOUCH_NT36532_H
#include <stddef.h>
#include <stdint.h>

namespace pianotouch {

// ------------------------------------------------------------------------------------------------------------ the transport
// One chip-select-framed full-duplex transfer of len bytes (rx null: TX only); 0 = done. maxWrite / maxRead: the data bytes one
// transfer may carry after its offset byte (GENI DMA: 63 KiB / 8 KiB; the GPIO transport: shorter chip-select windows).
struct Bus {
  void *ctx;
  int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, uint32_t len);
  void (*sleepMs)(void *ctx, unsigned ms);
  uint32_t maxWrite, maxRead;
};

enum : int { kOk = 0, kErrBus = -5, kErrParam = -22, kErrChip = -19, kErrImage = -8, kErrTimeout = -110, kErrState = -71 };

constexpr uint32_t kEngReset = 0x7fff80, kEnbCasc = 0x1fb12c, kSwrst = 0x1fb43e, kBootRdy = 0x1fb50d, kTxAutoCopyEn = 0x1fc925;
constexpr uint32_t kIlmDes = 0x1fb528, kIlmLength = 0x1fb518, kIlmGolden = 0x1fb500;
constexpr uint32_t kDlmDes = 0x1fb52c, kDlmLength = 0x1fb530, kDlmGolden = 0x1fb504;
constexpr uint32_t kEventCascadeE = 0x11c400, kEventSingle = 0x125800, kPollInfoCascadeE = 0x1093d8;
enum : uint8_t { kEvHostCmd = 0x50, kEvResetComplete = 0x60, kEvFwInfo = 0x78 };
enum : uint8_t { kResetStateInit = 0xa0, kResetStateCalibrated = 0xa3, kResetStateMax = 0xaf };
enum : uint8_t { kCmdOpMode = 0x01, kCmdStylus = 0x04, kCmdIdleBaseline = 0x19 };
// The idle-baseline command's value: 1 as the stock driver (nvt_xm_htc_set_idle_baseline_update) and piano's Ubuntu send it; 0 (sheng's,
// ShengNovatekTouch) was sent here until 2026-11-12.
constexpr uint16_t kIdleBaselineUpdate = 1;
struct HwRegs {
  uint32_t trim, swrst;
};
constexpr HwRegs kHwRegs[3] = {{0x1fb104, 0x1fb43e}, {0x3f004, 0x3f0fe}, {0x1f64e, 0x1f01a}};

constexpr uint32_t kTransportBytes = 257;          // frame[0..256]: the zero byte and the transport block
constexpr uint32_t kDefaultPayload = 5160, kMaxFrame = 8192, kMaxPayload = kMaxFrame - kTransportBytes - 1;
constexpr uint32_t kMaxPartitions = 32, kImageMin = 0x2000, kImageMax = 0x100000;

inline uint16_t le16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void put16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline void fill(uint8_t *p, uint8_t v, uint32_t n) { for (uint32_t i = 0; i < n; ++i) p[i] = v; }
inline void copy(uint8_t *d, const uint8_t *s, uint32_t n) { for (uint32_t i = 0; i < n; ++i) d[i] = s[i]; }

// ------------------------------------------------------------------------------------------------------------- the image
struct Partition {
  uint32_t bin, sram, size, crc;
};
struct Image {
  uint32_t bytes = 0;         // bounded by the "NVT"/"MOD" sector
  uint32_t count = 0;         // partitions: ILM, DLM, info, overlay
  bool cascade2nd = false;    // a cascaded second header: the download uses auto copy
  uint8_t version = 0;
  Partition part[kMaxPartitions] = {};
};

// The vendor's word-wise CRC-32 (polynomial 0x04c11db7, no reflection) over (len + 4) / 4 little-endian words; bytes beyond
// avail read as 0.
inline uint32_t crc32Words(const uint8_t *d, uint32_t len, uint32_t avail)
{
  uint32_t sum = 0xffffffffu;
  const uint32_t words = (len + 4) / 4;
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t w = 0;
    for (unsigned b = 0; b < 4; ++b)
      if (i * 4 + b < avail) w |= uint32_t(d[i * 4 + b]) << (8 * b);
    sum ^= w;
    for (unsigned j = 0; j < 32; ++j) sum = sum & 0x80000000u ? (sum << 1) ^ 0x04c11db7u : sum << 1;
  }
  return sum;
}

// 0, or kErrImage / kErrParam. Only sizes, addresses and CRCs are derived; no byte of the image is ever logged.
inline int parseImage(const uint8_t *d, uint32_t bytes, Image &im)
{
  im = Image();
  if (!d || bytes < kImageMin || bytes > kImageMax) return kErrParam;
  for (uint32_t s = bytes / 4096; s > 0 && !im.bytes; --s) {
    const uint8_t *e = d + s * 4096 - 3;
    if ((e[0] == 'N' && e[1] == 'V' && e[2] == 'T') || (e[0] == 'M' && e[1] == 'O' && e[2] == 'D')) im.bytes = s * 4096;
  }
  if (!im.bytes) return kErrImage;
  const uint32_t ver = im.bytes - 4096;
  if (uint8_t(d[ver] + d[ver + 1]) != 0xff) return kErrImage;
  im.version = d[ver];
  const uint32_t end = le32(d);
  if (end < 0x30 || end > bytes) return kErrImage;
  im.cascade2nd = (d[0x20] & 0x02) != 0;
  uint32_t info = 0, pos = 0x30;
  if (im.cascade2nd) {
    while (pos < end / 2) { ++info; pos += 0x10; }
    ++info;
  } else {
    while (pos < end) { ++info; pos += 0x10; }
  }
  const uint32_t overlay = d[0x28] & 0x10 ? d[0x28] & 0x0f : 0, count = 2 + overlay + info;
  if (count > kMaxPartitions) return kErrImage;
  bool header = false;
  pos = 0x30;
  for (uint32_t l = 0; l < count; ++l) {
    Partition &p = im.part[l];
    if (l < 2) {
      p.bin = le32(d + l * 12);
      p.sram = le32(d + 4 + l * 12);
      p.size = le32(d + 8 + l * 12);
      if (uint64_t(p.bin) + (p.size + 4) / 4 * 4 > bytes) return kErrImage;
      p.crc = crc32Words(d + p.bin, p.size, bytes - p.bin);
    } else {
      // A record after the header keeps the previous position unless a cascaded second header sits at end - 0x10.
      if (l < 2 + info) {
        if (!header) pos = 0x30 + 0x10 * (l - 2);
        else if (im.cascade2nd) pos = end - 0x10;
      } else {
        pos = im.part[1].bin + 0x10 * (l - 2 - info);
      }
      if (uint64_t(pos) + 16 > bytes) return kErrImage;
      p.sram = le32(d + pos);
      p.size = le32(d + pos + 4);
      p.bin = le32(d + pos + 8);
      p.crc = le32(d + pos + 12);
      if (l < 2 + info && p.bin < end && p.size) header = true;
    }
    if (uint64_t(p.bin) + p.size > bytes) return kErrImage;
  }
  if (!im.part[0].size || !im.part[1].size) return kErrImage;
  im.count = count;
  return kOk;
}

// --------------------------------------------------------------------------------------------------------------- the chip
struct Chip {
  Bus bus = {};
  uint8_t *tx = nullptr, *rx = nullptr;   // caller's buffers: tx >= maxWrite + 1 bytes, rx >= maxRead + 2 bytes
  uint32_t txBytes = 0, rxBytes = 0;
  // identify
  uint8_t id[6] = {};
  int table = -1, tries = 0;
  bool cascade = false, e = false;
  uint32_t eventBuf = 0, pollInfo = 0;
  // download evidence: the attempt and stage (1 reset, 2 bank, 3 auto copy on, 4 write, 5 auto copy wait, 6 CRC on, 7 boot,
  // 8 reset state) where the last one stopped, its result and the last bytes polled
  unsigned attempt = 0, stage = 0, polls = 0;
  int stageRc = 0;
  uint8_t resetState = 0, autoCopy = 0;
  uint32_t written = 0;
  // start
  uint32_t payload = kDefaultPayload, payloadReported = 0;
  uint8_t fwVersion = 0, xNum = 0, yNum = 0, calibrated = 0;
  bool fwInfoValid = false;
  int stylusRc = 1, baselineRc = 1, modeRc = 1;   // 1: not sent; else the host command's result
  // probeState: what it read last
  uint8_t probeVersion = 0, probeComplement = 0, probeReset = 0;
};

inline void sleepMs(Chip &c, unsigned ms) { if (c.bus.sleepMs && ms) c.bus.sleepMs(c.bus.ctx, ms); }
inline int raw(Chip &c, uint32_t len, bool read)
{
  return c.bus.xfer(c.bus.ctx, c.tx, read ? c.rx : nullptr, len) ? kErrBus : kOk;
}
inline int setPage(Chip &c, uint32_t addr)
{
  c.tx[0] = 0xff;
  c.tx[1] = uint8_t(addr >> 15);
  c.tx[2] = uint8_t(addr >> 7);
  return raw(c, 3, false);
}
// Writes len bytes from addr on, in transfers of at most maxWrite data bytes, each after its own page select.
inline int write(Chip &c, uint32_t addr, const uint8_t *data, uint32_t len)
{
  if (!len || c.txBytes < 2 || !c.bus.maxWrite) return kErrParam;
  const uint32_t chunk = c.bus.maxWrite < c.txBytes - 1 ? c.bus.maxWrite : c.txBytes - 1;
  for (uint32_t done = 0; done < len;) {
    const uint32_t n = len - done < chunk ? len - done : chunk;
    if (int r = setPage(c, addr + done)) return r;
    c.tx[0] = uint8_t(0x80 | ((addr + done) & 0x7f));
    copy(c.tx + 1, data + done, n);
    if (int r = raw(c, n + 1, false)) return r;
    done += n;
  }
  return kOk;
}
inline int writeByte(Chip &c, uint32_t addr, uint8_t v) { return write(c, addr, &v, 1); }
inline int write3(Chip &c, uint32_t addr, uint32_t v)
{
  const uint8_t b[3] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16)};
  return write(c, addr, b, 3);
}
inline int write4(Chip &c, uint32_t addr, uint32_t v)
{
  const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
  return write(c, addr, b, 4);
}
// Reads len bytes from addr on, in transfers of at most maxRead data bytes, each after its own page select.
inline int read(Chip &c, uint32_t addr, uint8_t *out, uint32_t len)
{
  if (!len || c.rxBytes < 3 || c.txBytes < 3 || !c.bus.maxRead) return kErrParam;
  uint32_t chunk = c.bus.maxRead;
  if (chunk > c.rxBytes - 2) chunk = c.rxBytes - 2;
  if (chunk > c.txBytes - 2) chunk = c.txBytes - 2;
  for (uint32_t done = 0; done < len;) {
    const uint32_t n = len - done < chunk ? len - done : chunk;
    if (int r = setPage(c, addr + done)) return r;
    fill(c.tx, 0, n + 2);
    c.tx[0] = uint8_t((addr + done) & 0x7f);
    if (int r = raw(c, n + 2, true)) return r;
    copy(out + done, c.rx + 2, n);
    done += n;
  }
  return kOk;
}

inline bool trimMatches(const uint8_t id[6], bool *e)
{
  if (id[3] != 0x32 || id[4] != 0x65 || id[5] != 0x03) return false;
  *e = id[0] == 0x0e;
  return true;
}

// Engine reset, then the trim-ID loop over the three hw-register tables (5 tries each); selects the memory map. kErrChip when no
// table answers with an NT36532 ID (c.id holds the last bytes read).
inline int identify(Chip &c)
{
  if (int r = writeByte(c, kEngReset, 0x5a)) return r;
  sleepMs(c, 11);
  c.table = -1;
  c.tries = 0;
  for (unsigned t = 0; t < 3 && c.table < 0; ++t)
    for (unsigned k = 0; k < 5; ++k) {
      ++c.tries;
      if (int r = writeByte(c, kHwRegs[t].swrst, 0x69)) return r;
      sleepMs(c, 5);
      uint8_t zero[6] = {};
      if (int r = write(c, kHwRegs[t].trim, zero, 6)) return r;
      if (int r = read(c, kHwRegs[t].trim, c.id, 6)) return r;
      if (trimMatches(c.id, &c.e)) {
        c.table = int(t);
        break;
      }
      sleepMs(c, 10);
    }
  if (c.table < 0) return kErrChip;
  uint8_t casc = 0;
  if (int r = read(c, kEnbCasc, &casc, 1)) return r;
  c.cascade = !(casc & 1);
  c.eventBuf = c.e && c.cascade ? kEventCascadeE : kEventSingle;
  c.pollInfo = c.e && c.cascade ? kPollInfoCascadeE : 0;
  return kOk;
}

inline int resetState(Chip &c, uint8_t *state)
{
  uint8_t b[5] = {};
  const int r = read(c, c.eventBuf + kEvResetComplete, b, 5);
  *state = b[0];
  return r;
}

// The host download with the hardware CRC check; kOk once the reset state is 0xa0..0xaf. Three attempts.
inline int download(Chip &c, const Image &im, const uint8_t *fw, uint32_t fwBytes)
{
  if (!c.eventBuf || im.count < 2 || !fw) return kErrParam;
  int rc = kErrTimeout;
  for (c.attempt = 1; c.attempt <= 3; ++c.attempt) {
    c.written = 0;
    c.stage = 1;
    if ((rc = writeByte(c, kSwrst, 0x69))) goto next;
    sleepMs(c, 5);
    c.stage = 2;
    if ((rc = write3(c, kIlmDes, im.part[0].sram)) || (rc = write3(c, kIlmLength, im.part[0].size)) ||
        (rc = write4(c, kIlmGolden, im.part[0].crc)) || (rc = write3(c, kDlmDes, im.part[1].sram)) ||
        (rc = write3(c, kDlmLength, im.part[1].size)) || (rc = write4(c, kDlmGolden, im.part[1].crc)))
      goto next;
    if (im.cascade2nd) {
      c.stage = 3;
      if ((rc = writeByte(c, kTxAutoCopyEn, 0x56))) goto next;
    }
    c.stage = 4;
    for (uint32_t i = 0; i < im.count; ++i) {
      const Partition &p = im.part[i];
      if (!p.size) continue;
      // size + 1 bytes, as the vendor flow writes them; a byte beyond the image goes out as 0.
      const uint32_t total = p.size + 1;
      for (uint32_t done = 0; done < total;) {
        uint32_t n = total - done;
        if (n > c.txBytes - 1) n = c.txBytes - 1;
        if (n > c.bus.maxWrite) n = c.bus.maxWrite;
        // write() would copy again: assemble the data in tx[1..] here and send it under one page select.
        if ((rc = setPage(c, p.sram + done))) goto next;
        c.tx[0] = uint8_t(0x80 | ((p.sram + done) & 0x7f));
        for (uint32_t k = 0; k < n; ++k) {
          const uint64_t at = uint64_t(p.bin) + done + k;
          c.tx[1 + k] = at < fwBytes ? fw[at] : 0;
        }
        if ((rc = raw(c, n + 1, false))) goto next;
        done += n;
        c.written += n;
      }
    }
    if (im.cascade2nd) {
      c.stage = 5;
      rc = kErrTimeout;
      for (c.polls = 1; c.polls <= 200; ++c.polls) {
        if (int r = read(c, kTxAutoCopyEn, &c.autoCopy, 1)) { rc = r; goto next; }
        if (!c.autoCopy) { rc = kOk; break; }
        sleepMs(c, 1);
      }
      if (rc) goto next;
    }
    c.stage = 6;
    {
      const uint8_t zero[6] = {}, crcOn[2] = {0xae, 0x00};
      if ((rc = write(c, c.eventBuf + kEvResetComplete, zero, 6)) || (rc = write(c, c.eventBuf + kEvHostCmd, crcOn, 2))) goto next;
    }
    c.stage = 7;
    if ((rc = writeByte(c, kBootRdy, 1))) goto next;
    sleepMs(c, 5);
    c.stage = 8;
    rc = kErrTimeout;
    for (c.polls = 1; c.polls <= 11; ++c.polls) {
      if (int r = resetState(c, &c.resetState)) { rc = r; goto next; }
      if (c.resetState >= kResetStateInit && c.resetState <= kResetStateMax) { rc = kOk; break; }
      sleepMs(c, 10);
    }
    if (!rc) {
      c.stageRc = 0;
      return kOk;
    }
  next:
    c.stageRc = rc;
  }
  c.attempt = 3;
  return rc ? rc : kErrTimeout;
}

// A host command {0xbf, cmd, 0, value LE16} at event + 0x50; kOk when the firmware has cleared its first byte (5 tries, 20 ms).
inline int hostCommand(Chip &c, uint8_t cmd, uint16_t value)
{
  sleepMs(c, 35);
  for (unsigned t = 0; t < 5; ++t) {
    const uint8_t b[5] = {0xbf, cmd, 0x00, uint8_t(value), uint8_t(value >> 8)};
    if (int r = write(c, c.eventBuf + kEvHostCmd, b, 5)) return r;
    sleepMs(c, 20);
    uint8_t st = 0xff;
    if (int r = read(c, c.eventBuf + kEvHostCmd, &st, 1)) return r;
    if (!st) return kOk;
  }
  return kErrTimeout;
}

// After the download: the payload length, the firmware info, the calibration wait (reset state 0xa3), then stylus scanning off
// (stylusOff), the idle baseline update (baseline) and the operating mode (mode <= 0xffff; > 0xffff: not sent), the order of the
// stock driver and of piano's Ubuntu ("disable unused stylus scanning before finger idle setup"). Neither the calibration nor a
// command that is not taken stops it (Project-SunUEFI, the sequence that ran on piano, sends no baseline command): their results are
// in c. kOk, or the first bus error.
inline int start(Chip &c, bool baseline, uint32_t mode, bool stylusOff = true)
{
  uint8_t b[40] = {};
  c.payload = kDefaultPayload;
  c.payloadReported = 0;
  if (c.pollInfo) {
    if (int r = read(c, c.pollInfo, b, 40)) return r;
    c.payloadReported = uint32_t(b[2]) | uint32_t(b[3]) << 8;
    if (c.payloadReported >= 64 && c.payloadReported <= kMaxPayload) c.payload = c.payloadReported;
  }
  if (int r = read(c, c.eventBuf + kEvFwInfo, b, 38)) return r;
  c.fwVersion = b[0];
  c.fwInfoValid = uint8_t(b[0] + b[1]) == 0xff;
  c.xNum = b[2];
  c.yNum = b[3];
  c.calibrated = 0;
  for (c.polls = 1; c.polls <= 50; ++c.polls) {
    if (int r = resetState(c, &c.resetState)) return r;
    if (c.resetState >= kResetStateCalibrated && c.resetState <= kResetStateMax) { c.calibrated = 1; break; }
    sleepMs(c, 10);
  }
  c.stylusRc = c.baselineRc = c.modeRc = 1;
  if (stylusOff && (c.stylusRc = hostCommand(c, kCmdStylus, 0)) == kErrBus) return kErrBus;
  if (baseline && (c.baselineRc = hostCommand(c, kCmdIdleBaseline, kIdleBaselineUpdate)) == kErrBus) return kErrBus;
  if (mode <= 0xffff && (c.modeRc = hostCommand(c, kCmdOpMode, uint16_t(mode))) == kErrBus) return kErrBus;
  return kOk;
}

// After a suspend, or the display back on (TouchPower.h): whether the controller still runs the image start() saw, by reads only:
// the firmware info valid (version + complement = 0xff) with start()'s version and x / y counts, and the reset state in
// 0xa0..0xaf. kOk with *kept; a bus error as it came.
inline int probeState(Chip &c, bool *kept)
{
  *kept = false;
  uint8_t b[4] = {};
  if (int r = read(c, c.eventBuf + kEvFwInfo, b, 4)) return r;
  if (int r = resetState(c, &c.probeReset)) return r;
  c.probeVersion = b[0];
  c.probeComplement = b[1];
  *kept = c.fwInfoValid && uint8_t(b[0] + b[1]) == 0xff && b[0] == c.fwVersion && b[2] == c.xNum && b[3] == c.yNum &&
          c.probeReset >= kResetStateInit && c.probeReset <= kResetStateMax;
  return kOk;
}

inline uint32_t frameBytes(const Chip &c) { return kTransportBytes + c.payload; }
// frame[0] = 0, frame[1..] = the event buffer from its first byte (the vendor driver's layout, which the packet offsets are on).
inline int readFrame(Chip &c, uint8_t *frame)
{
  frame[0] = 0;
  return read(c, c.eventBuf, frame + 1, frameBytes(c) - 1);
}

// ------------------------------------------------------------------------------------------------------------- the frames
enum class FrameKind : uint8_t { data, handshake, watchdog };
inline FrameKind frameKind(const uint8_t *frame)
{
  bool hand = true, wdt = true;
  for (unsigned i = 1; i < 7; ++i) {
    if (frame[i] != 0x77) hand = false;
    if (frame[i] != 0xfd && frame[i] != 0xfe) wdt = false;
  }
  return hand ? FrameKind::handshake : wdt ? FrameKind::watchdog : FrameKind::data;
}
// 1 the checksum matches, 0 it does not, -1 the length fields are inconsistent (or beyond len).
inline int packetChecksum(const uint8_t *p, uint32_t len)
{
  if (!p || len < 20) return -1;
  const uint16_t sum16 = le16(p + 4), inv16 = le16(p + 12);
  const uint32_t words = le32(p + 8), invWords = le32(p + 16);
  if (inv16 != uint16_t(~sum16) || invWords != ~words) return -1;
  if (!words || words > (len - 20) / 4) return -1;
  uint32_t s = 0;
  for (uint32_t i = 0; i < words * 2; ++i) s += le16(p + 20 + i * 2);
  return uint16_t(~s + 1) == sum16 ? 1 : 0;
}
struct PacketFields {
  uint8_t marker, scanMode, cols, rows, type;
  uint16_t counter, frameNo;
};
inline PacketFields packetFields(const uint8_t *p)
{
  return PacketFields{p[0], p[24], p[48], p[49], p[56], le16(p + 2), le16(p + 28)};
}

}  // namespace pianotouch
#endif
