// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 5: the WLAN firmware's QMI service (WLFW, service 69 version 1 instance 1 on this chip family) as one portable
// client the owner steps, with a simulated firmware in the host tests (tests/qmi_host.cpp). New code for this repository: the
// message identifiers, the tag of every field and the order the firmware expects them in are facts of the service (docs/drivers/
// wifi.md section 12 has the table of every message with its tags), and the Linux driver of this chip was read for those facts
// only — no code, comment or table of it is here.
//
// The exchange, in the order the firmware wants it (each step waits for its answer; a step that fails ends level 5 where it is):
//   502 capability of the radio    request 0x0057, empty            -> how many radios, the board identifier (optional: a failure
//                                                                     here is not fatal, two links are then assumed)
//   503 register for indications   request 0x0020                   -> the firmware may now send us its three indications
//   504 host capability            request 0x0034                   -> what this host is: one client, the chip's internal sleep
//                                                                     clock, board-file and microcode support, the feature list
//   505 the firmware asks for memory   indication 0x0035            <- a list of regions it wants in our memory
//   506 answer with the addresses  request 0x0036                   -> one address per region (or "ask again in smaller pieces")
//   507 the firmware's memory is ready indication 0x0037            <-
//   508 capability of the chip     request 0x0024, empty            -> chip, board and firmware identities
//   509 the regulatory database    request 0x0025 in 6 KiB segments -> regdb_xiaomi.bin from the stock partition
//   510 the board file             request 0x0025 in 6 KiB segments -> bd_p81.elf (China-region, this unit's own)
//   511 the PHY microcode          request 0x003c {address, size}   -> phy_ucode20.elf, in our window
//   512 the auxiliary microcode    request 0x005a {address, size}   -> aux_ucode20.elf, in our window (this chip asks for it)
//   513 the trace configuration    request 0x0044 in 6 KiB segments -> qdss_trace_config_v2.cfg (this chip does not set the
//                                                                     "configuration missing" feature bit, so it is sent; a
//                                                                     refusal, or no such file, is noted and the exchange goes on:
//                                                                     the trace is a debugging aid, not part of the firmware's start)
//   514 the firmware is ready      indication 0x0038                <- **level 5's gate**
//   515 the firmware's own options request 0x002f                   -> firmware logging on        (only with piano-wifi-mode=1)
//   516 the host's configuration   request 0x0023                   -> the copy engine's pipes, the services' pipes, and the
//                                                                     shadow registers when piano-wifi-shadow=1
//   517 the mode                   request 0x0022 {normal}          -> the firmware starts its radio; level 6's doorway
// No file is ever held whole: a download segment is read from the stock partition straight into the message being built, and the
// two microcodes are read straight into the window the chip reads them from.
//
// The file identifier every download carries is the board identifier of the chip's capability answer (its OTP's), or 0xff when it
// gives none, as the chip's own driver does; the radio's capability carries a board identifier as well, which that driver only
// logs, and so does this one. With 0xff (an unprogrammed OTP, this unit) that driver finds no entry in a board-2 container (the
// stock image has none) and falls back to its single API-1 board file, which for this tablet is the stock China-region bd_p81.elf:
// wlfwBoardFile says so for the log. Every response and indication goes to the owner's `event` callback with its result and error
// codes and, for a download, which segment it answered.
#pragma once
#include <stdint.h>
#include "WifiHw.h"
#include "WifiQmi.h"

namespace pianowifi {

enum WlfwStep : int {
  kWlfwStepIdle = 500, kWlfwStepPhyCap = 502, kWlfwStepIndRegister = 503, kWlfwStepHostCap = 504, kWlfwStepMemRequest = 505,
  kWlfwStepRespondMem = 506, kWlfwStepMemReady = 507, kWlfwStepChipCap = 508, kWlfwStepRegdb = 509, kWlfwStepBoard = 510,
  kWlfwStepM3 = 511, kWlfwStepAuxUc = 512, kWlfwStepQdss = 513, kWlfwStepFwReady = 514, kWlfwStepIni = 515, kWlfwStepCfg = 516,
  kWlfwStepMode = 517, kWlfwStepDone = 518, kWlfwStepStopped = 519,
};

enum WlfwError : int {
  kWlfwErrNone = 0, kWlfwErrNoBuffer = -1, kWlfwErrSend = -2, kWlfwErrMessage = -3, kWlfwErrResult = -4, kWlfwErrFile = -5,
  kWlfwErrMemory = -6, kWlfwErrSegments = -7, kWlfwErrState = -8, kWlfwErrTimeout = -9, kWlfwErrBuild = -10,
};

inline const char *wlfwStepName(int step)
{
  switch (step) {
  case kWlfwStepIdle: return "idle";
  case kWlfwStepPhyCap: return "the radio's capability";
  case kWlfwStepIndRegister: return "registering for the firmware's indications";
  case kWlfwStepHostCap: return "this host's capability";
  case kWlfwStepMemRequest: return "waiting for the firmware's memory request";
  case kWlfwStepRespondMem: return "answering the memory request";
  case kWlfwStepMemReady: return "waiting for the firmware's memory to be ready";
  case kWlfwStepChipCap: return "the chip's capability";
  case kWlfwStepRegdb: return "the regulatory database";
  case kWlfwStepBoard: return "the board file";
  case kWlfwStepM3: return "the PHY microcode";
  case kWlfwStepAuxUc: return "the auxiliary microcode";
  case kWlfwStepQdss: return "the trace configuration";
  case kWlfwStepFwReady: return "waiting for the firmware to be ready";
  case kWlfwStepIni: return "the firmware's options";
  case kWlfwStepCfg: return "the host's configuration";
  case kWlfwStepMode: return "the mode";
  case kWlfwStepDone: return "done";
  default: return "stopped";
  }
}

inline const char *wlfwErrorName(int error)
{
  switch (error) {
  case kWlfwErrNone: return "none";
  case kWlfwErrNoBuffer: return "no transport buffer was free";
  case kWlfwErrSend: return "the transport refused the message";
  case kWlfwErrMessage: return "a message did not parse";
  case kWlfwErrResult: return "the firmware refused a request";
  case kWlfwErrFile: return "a firmware file could not be read";
  case kWlfwErrMemory: return "the window has no room for what the firmware asked for";
  case kWlfwErrSegments: return "the firmware asked for more regions than this driver answers";
  case kWlfwErrState: return "a message arrived in the wrong step";
  case kWlfwErrTimeout: return "a step timed out";
  default: return "a message could not be built";
  }
}

inline const char *wlfwMemTypeName(uint32_t type)
{
  switch (type) {
  case kMemHostDdr: return "host memory";
  case kMemBdf: return "the board file's";
  case kMemM3Dump: return "the PHY microcode's dump";
  case kMemCaldb: return "the calibration database";
  case kMemMloGlobal: return "the multi-link global";
  case kMemPageable: return "pageable";
  case kMemLpassShared: return "shared with the audio DSP";
  default: return "a kind this driver does not know";
  }
}

// The region types this driver answers with memory of its own window; any other type is answered with the address 0 and its size
// kept, which the firmware accepts (it then does without that region).
inline bool wlfwMemWanted(uint32_t type)
{
  return type == kMemHostDdr || type == kMemM3Dump || type == kMemCaldb || type == kMemPageable || type == kMemLpassShared;
}

// ------------------------------------------------------------------------------------------------- the shadow register list
// A ring whose head pointer the firmware may shadow: the register's offset inside the chip's register space, and whether it is a
// destination ring (whose tail pointer is the head pointer's offset plus four).
struct ShadowRing {
  uint32_t base = 0;        // the ring's head-pointer register (its unit's base plus the offset in it)
  uint32_t stride = 0;      // between two rings of this kind
  uint32_t count = 0;
  bool destination = false;
};
// The list as the firmware reads it: one address per ring, in the order the host created them. Returns how many were written
// (never more than the chip's 40 shadow registers, nor the capacity given).
inline unsigned shadowList(uint32_t *out, unsigned capacity, const ShadowRing *rings, unsigned count)
{
  unsigned n = 0;
  if (capacity > kShadowRegisters) capacity = kShadowRegisters;
  for (unsigned i = 0; i < count && n < capacity; ++i)
    for (uint32_t r = 0; r < rings[i].count && n < capacity; ++r)
      out[n++] = rings[i].base + rings[i].stride * r + (rings[i].destination ? kShadowHpToTp : 0);
  return n;
}
// The rings level 6 plans to create, as the chip's register map has them (WifiHw.h). Level 5 sends this list only with
// piano-wifi-shadow=1: until level 6 creates the rings, a shadow register stands for nothing.
constexpr ShadowRing kShadowRingsPlanned[] = {
  {kUnitReo + kReo1RingHp, kReo2RingHp - kReo1RingHp, 8, true},    // the receive reorder's destination rings
  {kUnitReo + kReoSw0RingHp, 0, 1, true},                          // its exception ring
  {kUnitReo + kSw2ReoHp, 0, 1, false},                             // reinjection
  {kUnitReo + kReoCmdHp, 0, 1, false},
  {kUnitReo + kReoStatusHp, 0, 1, true},
  {kUnitTcl + kTcl1RingHp, kTcl2RingHp - kTcl1RingHp, 5, false},   // the transmit command rings
  {kUnitTcl + kTclRingHp, 0, 1, false},
  {kUnitTcl + kTclStatusHp, 0, 1, true},
  {kUnitWbm + kWbmIdleLinkHp, 0, 1, false},
  {kUnitWbm + kWbmSwReleaseHp, 0, 1, false},
  {kUnitWbm + kWbm0ReleaseHp, kWbm1ReleaseHp - kWbm0ReleaseHp, 8, true},
  {kUnitCe0Src + kCeDstRingHp, kUnitCe1Src - kUnitCe0Src, kCePipeCount, false},
  {kUnitCe0Dst + kCeDstRingHp, kUnitCe1Dst - kUnitCe0Dst, kCePipeCount, false},
  {kUnitCe0Dst + kCeDstStatusRingHp, kUnitCe1Dst - kUnitCe0Dst, kCePipeCount, true},
};
constexpr unsigned kShadowRingsPlannedCount = sizeof kShadowRingsPlanned / sizeof kShadowRingsPlanned[0];

// ------------------------------------------------------------------------------------------------------------- the client
struct WlfwMemRegion {
  uint32_t size = 0, type = 0;
  uint64_t address = 0;        // what the firmware was given (0: a type this driver does not answer with memory)
  uint8_t *va = nullptr;
};

// What the firmware said, one message at a time, for the owner's log: a response with its result and error codes (and, when it
// answers a download segment, which one), or an indication.
enum WlfwEventKind : uint8_t { kWlfwEventResponse = 0, kWlfwEventIndication = 1, kWlfwEventUnexpected = 2 };
struct WlfwEvent {
  int step = 0;                 // the step the client was in when it came
  uint8_t kind = kWlfwEventResponse;
  uint16_t message = 0, transaction = 0;
  bool hasResult = false;       // a response without the result TLV is itself a failure (except the radio's capability)
  uint16_t result = 0, error = 0;
  int32_t segment = -1;         // the download segment it answers (from 0), -1 when it answers no download
  uint32_t segments = 0, bytes = 0;
  bool last = false;            // that segment carried the end flag
  const char *file = nullptr;   // the file of that download
};

// The file the board-file step sends for a board identifier, and why (for the log). The stock image carries one board file for this
// tablet's region and no board-2 container, so every identifier gets it; 0xff is the identifier of a chip whose OTP names no board,
// for which the chip's own driver takes its API-1 default board file, which is this one.
inline const char *wlfwBoardFile(uint32_t boardId, const char **why)
{
  if (why)
    *why = boardId == kBoardIdDefault
             ? "the chip names no board (0xff: its OTP carries no board identifier), so the stock API-1 default board file, as the "
               "chip's own driver chooses with no board-2 container (the stock image has none)"
             : "the stock image's only board file for this tablet's region (no board-2 container to choose a variant from)";
  return kBoardName;
}

struct WlfwIo {
  void *ctx = nullptr;
  // A transport buffer to build the next message in, and its capacity; null when none is free.
  uint8_t *(*claim)(void *ctx, uint32_t *capacity) = nullptr;
  // The message in that buffer, `length` bytes: the owner puts the router header in front and gives it to the chip. 0 when queued.
  int (*send)(void *ctx, uint8_t *buffer, uint32_t length) = nullptr;
  // The size of a firmware file on the stock partition (0: it is not there), and `bytes` of it at `offset` into `to`.
  uint32_t (*file_size)(void *ctx, const char *name) = nullptr;
  int (*file_read)(void *ctx, const char *name, uint64_t offset, void *to, uint32_t bytes) = nullptr;
  // `bytes` of the window for the chip to read or write: the address the chip must use, 0 when there is no room.
  uint64_t (*memory)(void *ctx, uint32_t bytes, uint8_t **va) = nullptr;
  void (*step)(void *ctx, int step, const char *what) = nullptr;
  // Every response that belongs to the request outstanding, and every indication, as it arrives (optional).
  void (*event)(void *ctx, const WlfwEvent &event) = nullptr;
};

struct WlfwClient {
  WlfwIo io;
  int step = kWlfwStepIdle, error = kWlfwErrNone, failedStep = 0;
  uint16_t transaction = 0, pending = 0, pendingMessage = 0;
  uint16_t result = 0, resultError = 0;
  bool waiting = false;                  // a request is out and its answer has not come
  unsigned mode = 0, shadow = 0;         // piano-wifi-mode, piano-wifi-shadow
  // what the firmware told us
  uint8_t radios = 0, macs = 0;
  bool singleChipMlo = false;
  uint32_t boardId = kBoardIdDefault, chipId = 0, chipFamily = 0, socId = 0, fwVersion = 0, otpVersion = 0, voltageMv = 0;
  // the two board identifiers as given: the radio's capability (only logged) and the chip's (the file identifier)
  uint32_t phyBoardId = 0, capBoardId = 0;
  bool havePhyBoard = false, haveCapBoard = false;
  uint64_t fwStatus = 0, fwCaps = 0;
  char buildTimestamp[33] = {};
  // the memory the firmware asked for
  WlfwMemRegion regions[kWlfwMaxMemSegments];
  uint32_t regionCount = 0, regionBytes = 0, regionGiven = 0, memRequests = 0;
  bool memDelayed = false;
  // The firmware does not have to wait for our answer before it speaks: an indication may arrive while the response of the step
  // before it is still outstanding. Each of the three is remembered, and the step that waits for it then finds it already there.
  bool pendingMemRequest = false, pendingMemReady = false, pendingFwReady = false;
  // the file being downloaded
  const char *fileName = nullptr;
  uint32_t fileBytes = 0, fileOffset = 0, segmentId = 0;
  uint8_t fileType = 0;
  // the request outstanding, as the log names it: a download's segment (-1: none), its size, its end flag
  int32_t outSegment = -1;
  uint32_t outSegments = 0, outBytes = 0;
  bool outLast = false;
  const char *outFile = nullptr;
  bool timedOutWaiting = false;          // the step that timed out had a request out (its fields above say which)
  // the trace configuration's outcome: refused (with the codes) or skipped (no file); neither stops the exchange
  bool qdssRefused = false, qdssSkipped = false, qdssTimedOut = false;
  uint16_t qdssResult = 0, qdssError = 0;
  // counters for the log
  unsigned requests = 0, responses = 0, indications = 0, segments = 0, unexpected = 0;
  uint32_t shadowCount = 0;
  uint32_t shadowRegisters[kShadowRegisters] = {};
};

namespace wlfwdetail {

inline int fail(WlfwClient &c, int error)
{
  c.failedStep = c.step;
  c.error = error;
  c.step = kWlfwStepStopped;
  c.waiting = false;
  return c.error;
}

// One request out of a claimed buffer. `build(writer)` fills the TLVs; the transaction is remembered, so that only its own answer
// moves the client on.
template <class Build>
inline bool request(WlfwClient &c, uint16_t message, Build build)
{
  uint32_t capacity = 0;
  uint8_t *buffer = c.io.claim ? c.io.claim(c.io.ctx, &capacity) : nullptr;
  if (!buffer || !capacity) {
    fail(c, kWlfwErrNoBuffer);
    return false;
  }
  if (!++c.transaction) c.transaction = 1;
  QmiWriter w;
  qmiBegin(w, buffer, capacity, kQmiRequest, c.transaction, message);
  build(w);
  const uint32_t length = qmiEnd(w);
  if (!length) {
    fail(c, kWlfwErrBuild);
    return false;
  }
  if (c.io.send(c.io.ctx, buffer, length)) {
    fail(c, kWlfwErrSend);
    return false;
  }
  c.pending = c.transaction;
  c.pendingMessage = message;
  c.waiting = true;
  c.outSegment = -1;                     // a download says which segment this is right after (download below)
  c.outSegments = c.outBytes = 0;
  c.outLast = false;
  c.outFile = nullptr;
  ++c.requests;
  return true;
}

// A segment of a file straight into the message being built: the data TLV's two-byte count, then the bytes themselves read out of
// the stock partition. `read` tells a build failure (the buffer) from a read failure (the partition).
inline void dataTlv(WlfwClient &c, QmiWriter &w, uint8_t tag, const char *name, uint32_t offset, uint32_t take, bool *read)
{
  qmiTlvBegin(w, tag);
  qmiLe(w, take, 2);
  if (w.overflow || take > w.capacity - w.length) {
    w.overflow = true;
  } else if (c.io.file_read(c.io.ctx, name, offset, w.buffer + w.length, take)) {
    *read = false;
    w.overflow = true;
  } else {
    w.length += take;
  }
  qmiTlvEnd(w);
}

// The next segment of the download the client is in the middle of.
inline bool download(WlfwClient &c)
{
  const bool board = c.step == kWlfwStepBoard || c.step == kWlfwStepRegdb;
  const uint32_t left = c.fileBytes - c.fileOffset;
  const uint32_t take = left > kWlfwMaxDataBytes ? kWlfwMaxDataBytes : left;
  const uint32_t offset = c.fileOffset, segment = c.segmentId, id = c.boardId;
  const uint8_t type = c.fileType, last = take == left ? 1 : 0;
  const char *name = c.fileName;
  bool read = true;
  const bool ok = request(c, board ? uint16_t(kWlfwBdfDownload) : uint16_t(kWlfwQdssDownload), [&](QmiWriter &w) {
    if (board) {
      qmiU8(w, 0x01, 1);                       // this message carries data
      qmiU32(w, 0x10, id);                     // the file identifier: the board identifier the chip reported
      qmiU32(w, 0x11, left);                   // what is still to come, this segment included
      qmiU32(w, 0x12, segment);
      dataTlv(c, w, 0x13, name, offset, take, &read);
      qmiU8(w, 0x14, last);
      qmiU8(w, 0x15, type);
    } else {
      qmiU32(w, 0x10, left);
      qmiU32(w, 0x11, segment);
      dataTlv(c, w, 0x12, name, offset, take, &read);
      qmiU8(w, 0x13, last);
    }
  });
  if (!ok) {
    if (!read) fail(c, kWlfwErrFile);
    return false;
  }
  c.outSegment = int32_t(segment);
  c.outSegments = (c.fileBytes + kWlfwMaxDataBytes - 1) / kWlfwMaxDataBytes;
  c.outBytes = take;
  c.outLast = last != 0;
  c.outFile = name;
  c.fileOffset += take;
  ++c.segmentId;
  ++c.segments;
  return true;
}

inline bool startDownload(WlfwClient &c, const char *name, uint8_t type, uint32_t limit)
{
  const uint32_t bytes = c.io.file_size ? c.io.file_size(c.io.ctx, name) : 0;
  if (!bytes) {
    fail(c, kWlfwErrFile);
    return false;
  }
  c.fileName = name;
  c.fileType = type;
  c.fileBytes = limit && bytes > limit ? limit : bytes;
  c.fileOffset = 0;
  c.segmentId = 0;
  return download(c);
}

// A file read into the window, whose address the firmware is then given (the two microcodes).
inline bool byAddress(WlfwClient &c, uint16_t message, const char *name)
{
  const uint32_t bytes = c.io.file_size ? c.io.file_size(c.io.ctx, name) : 0;
  if (!bytes) {
    fail(c, kWlfwErrFile);
    return false;
  }
  uint8_t *va = nullptr;
  const uint64_t address = c.io.memory(c.io.ctx, bytes, &va);
  if (!address || !va) {
    fail(c, kWlfwErrMemory);
    return false;
  }
  if (c.io.file_read(c.io.ctx, name, 0, va, bytes)) {
    fail(c, kWlfwErrFile);
    return false;
  }
  return request(c, message, [&](QmiWriter &w) {
    qmiU64(w, 0x01, address);
    qmiU32(w, 0x02, bytes);
  });
}

}  // namespace wlfwdetail

// The configuration message's TLVs, so that the host tests can read them back on their own: the host version string, the chip's
// nine copy-engine pipes, the fourteen service-to-pipe entries, and the shadow registers when there are any.
inline void wlfwBuildCfg(QmiWriter &w, const uint32_t *shadow, unsigned shadowCount)
{
  qmiString(w, 0x10, kHostVersionString, 16);
  qmiTlvBegin(w, 0x11);
  qmiLe(w, kCePipeCount, 1);
  for (const CePipeConfig &pipe : kCePipes) {
    qmiLe(w, pipe.pipe, 4);
    qmiLe(w, pipe.dir, 4);
    qmiLe(w, pipe.entries, 4);
    qmiLe(w, pipe.bytesMax, 4);
    qmiLe(w, pipe.flags, 4);
  }
  qmiTlvEnd(w);
  qmiTlvBegin(w, 0x12);
  qmiLe(w, kCeServiceCount, 1);
  for (const CeServiceConfig &service : kCeServices) {
    qmiLe(w, service.service, 4);
    qmiLe(w, service.dir, 4);
    qmiLe(w, service.pipe, 4);
  }
  qmiTlvEnd(w);
  if (shadowCount && shadow) {
    qmiTlvBegin(w, 0x17);
    qmiLe(w, shadowCount, 1);
    for (unsigned i = 0; i < shadowCount; ++i) qmiLe(w, shadow[i], 4);
    qmiTlvEnd(w);
  }
}

// The answer to the firmware's memory request: one entry per region with the address it was given, or — when a region this driver
// does answer did not fit the window and is larger than the service's chunk limit — the empty form, which asks the firmware to
// request the same memory again in smaller pieces (its own driver's fallback, and the only one that can work here: the window is
// one contiguous range this driver never grows).
inline void wlfwBuildRespondMem(QmiWriter &w, const WlfwClient &c)
{
  qmiTlvBegin(w, 0x01);
  qmiLe(w, c.memDelayed ? 0u : c.regionCount, 1);
  if (!c.memDelayed)
    for (uint32_t i = 0; i < c.regionCount; ++i) {
      qmiLe(w, c.regions[i].address, 8);
      qmiLe(w, c.regions[i].size, 4);
      qmiLe(w, c.regions[i].type, 4);
      qmiLe(w, 0, 1);                      // restore: this driver never reuses a region across a firmware restart
    }
  qmiTlvEnd(w);
}

// Starts the exchange: the step machine's first request. One exchange per boot (a restart would mean a firmware that has been
// reloaded, which level 5 does not do).
inline bool wlfwStart(WlfwClient &c, const WlfwIo &io, unsigned mode, unsigned shadow)
{
  if (c.step != kWlfwStepIdle) {
    wlfwdetail::fail(c, kWlfwErrState);
    return false;
  }
  c = WlfwClient{};
  c.io = io;
  c.mode = mode;
  c.shadow = shadow;
  c.step = kWlfwStepPhyCap;
  if (c.io.step) c.io.step(c.io.ctx, c.step, wlfwStepName(c.step));
  return wlfwdetail::request(c, kWlfwPhyCap, [](QmiWriter &) {});
}

// Enters the next step and sends its request (the steps that wait for an indication send nothing and simply stand).
inline bool wlfwEnter(WlfwClient &c, int step);

inline bool wlfwEnter(WlfwClient &c, int step)
{
  using namespace wlfwdetail;
  c.step = step;
  c.waiting = false;
  if (c.io.step) c.io.step(c.io.ctx, step, wlfwStepName(step));
  switch (step) {
  case kWlfwStepIndRegister:
    return request(c, kWlfwIndRegister, [](QmiWriter &w) {
      qmiU8(w, 0x10, 1);                      // tell us when the firmware is ready
      qmiU32(w, 0x15, kWlfwClientId);         // who we are
      qmiU8(w, 0x16, 1);                      // tell us when you want memory
      qmiU8(w, 0x17, 1);                      // tell us when that memory is ready
      qmiU8(w, 0x18, 1);                      // tell us when initialisation is done
      qmiU8(w, 0x1b, 1);                      // tell us when calibration is done
    });
  case kWlfwStepHostCap:
    return request(c, kWlfwHostCap, [](QmiWriter &w) {
      qmiU32(w, 0x10, 1);                     // one client of this firmware
      qmiU8(w, 0x13, kHostCapNmModem);        // this host's C-states, its internal sleep clock, its global PCIe reset
      qmiU8(w, 0x14, 1);                      // the host downloads the board file
      qmiU8(w, 0x16, 1);                      // the host downloads the PHY microcode
      qmiU8(w, 0x17, 1);                      // and keeps it in memory of its own
      qmiU8(w, 0x1a, 0);                      // no calibration has been done
      qmiU8(w, 0x1c, kWlfwMemCfgModeDefault); // the full-size memory configuration
      qmiU64(w, 0x27, kWlfwFeatureList);      // no PERST# pull on reset; the auxiliary microcode is supported
    });
  case kWlfwStepRespondMem:
    c.pendingMemRequest = false;
    return request(c, kWlfwRespondMem, [&c](QmiWriter &w) { wlfwBuildRespondMem(w, c); });
  case kWlfwStepChipCap:
    return request(c, kWlfwCap, [](QmiWriter &) {});
  case kWlfwStepRegdb:
    return startDownload(c, kRegdbName, kBdfTypeRegdb, 0);
  case kWlfwStepBoard: {
    // The kind is the file's own: an ELF board file (the stock one is) is kind 1, anything else the binary kind 0, as the chip's
    // own driver tells them apart by the first four bytes.
    const char *name = wlfwBoardFile(c.boardId, nullptr);
    uint8_t magic[4] = {};
    const bool elf = c.io.file_read && c.io.file_size && c.io.file_size(c.io.ctx, name) >= sizeof magic &&
                     !c.io.file_read(c.io.ctx, name, 0, magic, sizeof magic) && magic[0] == 0x7f && magic[1] == 'E' &&
                     magic[2] == 'L' && magic[3] == 'F';
    return startDownload(c, name, elf ? kBdfTypeElf : kBdfTypeBin, kBoardFileLimit);
  }
  case kWlfwStepM3:
    return byAddress(c, kWlfwM3Info, kM3Name);
  case kWlfwStepAuxUc:
    return byAddress(c, kWlfwAuxUcInfo, kAuxUcName);
  case kWlfwStepQdss:
    if (!c.io.file_size || !c.io.file_size(c.io.ctx, kQdssName)) {   // no trace configuration: the firmware starts without one
      c.qdssSkipped = true;
      return wlfwEnter(c, kWlfwStepFwReady);
    }
    return startDownload(c, kQdssName, 0, 0);
  case kWlfwStepIni:
    return request(c, kWlfwWlanIni, [](QmiWriter &w) { qmiU8(w, 0x10, 1); });   // the firmware's own log on
  case kWlfwStepCfg:
    c.shadowCount = c.shadow ? shadowList(c.shadowRegisters, kShadowRegisters, kShadowRingsPlanned, kShadowRingsPlannedCount) : 0;
    return request(c, kWlfwWlanCfg, [&c](QmiWriter &w) { wlfwBuildCfg(w, c.shadowRegisters, c.shadowCount); });
  case kWlfwStepMode:
    return request(c, kWlfwWlanMode, [](QmiWriter &w) {
      qmiU32(w, 0x01, kFirmwareModeNormal);
      qmiU8(w, 0x10, 0);                      // no hardware debugging
    });
  case kWlfwStepMemRequest:
    if (!c.pendingMemRequest) return true;    // nothing to send: the firmware speaks next
    c.pendingMemRequest = false;
    return wlfwEnter(c, kWlfwStepRespondMem);
  case kWlfwStepMemReady:
    if (!c.pendingMemReady) return true;
    c.pendingMemReady = false;
    return wlfwEnter(c, kWlfwStepChipCap);
  case kWlfwStepFwReady:
    if (!c.pendingFwReady) return true;
    c.pendingFwReady = false;
    return wlfwEnter(c, c.mode ? kWlfwStepIni : kWlfwStepDone);
  case kWlfwStepDone:
    return true;
  default:
    fail(c, kWlfwErrState);
    return false;
  }
}

namespace wlfwdetail {

// A response's result, and the step that follows it.
inline bool resultOk(WlfwClient &c, const QmiMessage &m)
{
  if (!qmiResult(m, &c.result, &c.resultError)) {
    fail(c, kWlfwErrMessage);
    return false;
  }
  return c.result == kQmiResultSuccess;
}

inline void readPhyCap(WlfwClient &c, const QmiMessage &m)
{
  uint8_t radios = 0, mlo = 0;
  uint32_t board = 0;
  if (qmiReadU8(m, 0x10, &radios) && radios) c.radios = radios;
  if (qmiReadU32(m, 0x11, &board)) {     // only noted: the file identifier is the chip capability's board (readChipCap)
    c.phyBoardId = board;
    c.havePhyBoard = true;
  }
  if (qmiReadU8(m, 0x13, &mlo)) c.singleChipMlo = mlo != 0;
}

inline void readChipCap(WlfwClient &c, const QmiMessage &m)
{
  QmiTlvReader chip = qmiTlvReader(m, 0x10);
  if (!chip.bad) {
    c.chipId = uint32_t(qmiTake(chip, 4));
    c.chipFamily = uint32_t(qmiTake(chip, 4));
  }
  QmiTlvReader board = qmiTlvReader(m, 0x11);
  c.boardId = kBoardIdDefault;           // no board information: the default identifier, as the chip's own driver uses
  if (!board.bad) {
    const uint32_t id = uint32_t(qmiTake(board, 4));
    if (!board.bad) {
      c.boardId = c.capBoardId = id;
      c.haveCapBoard = true;
    }
  }
  QmiTlvReader soc = qmiTlvReader(m, 0x12);
  if (!soc.bad) c.socId = uint32_t(qmiTake(soc, 4));
  QmiTlvReader version = qmiTlvReader(m, 0x13);
  if (!version.bad) {
    c.fwVersion = uint32_t(qmiTake(version, 4));
    const uint32_t n = uint32_t(qmiTake(version, 1));   // a string inside a structure carries its own length
    for (uint32_t i = 0; i < n && i + 1 < sizeof c.buildTimestamp && !version.bad; ++i)
      c.buildTimestamp[i] = char(qmiTake(version, 1));
  }
  qmiReadU8(m, 0x15, &c.macs);
  qmiReadU32(m, 0x16, &c.voltageMv);
  qmiReadU32(m, 0x18, &c.otpVersion);
  qmiReadU64(m, 0x1a, &c.fwCaps);
}

// The firmware's memory request: the regions it wants, each answered out of the owner's window.
inline bool readMemRequest(WlfwClient &c, const QmiMessage &m)
{
  QmiTlvReader r = qmiTlvReader(m, 0x01);
  if (r.bad) return fail(c, kWlfwErrMessage), false;
  const uint32_t count = uint32_t(qmiTake(r, 1));
  if (r.bad || !count || count > kWlfwMaxMemSegments) return fail(c, kWlfwErrSegments), false;
  ++c.memRequests;
  c.regionCount = 0;
  c.regionBytes = 0;
  c.regionGiven = 0;
  c.memDelayed = false;
  for (uint32_t i = 0; i < count; ++i) {
    WlfwMemRegion region;
    region.size = uint32_t(qmiTake(r, 4));
    region.type = uint32_t(qmiTake(r, 4));
    const uint32_t configs = uint32_t(qmiTake(r, 1));
    if (r.bad || configs > kWlfwMaxMemCfg) return fail(c, kWlfwErrMessage), false;
    for (uint32_t k = 0; k < configs; ++k) {   // the firmware's own view of the region: read past, nothing of it is ours
      (void)qmiTake(r, 8);
      (void)qmiTake(r, 4);
      (void)qmiTake(r, 1);
    }
    if (r.bad) return fail(c, kWlfwErrMessage), false;
    c.regionBytes += region.size;
    if (region.size && wlfwMemWanted(region.type)) {
      region.address = c.io.memory(c.io.ctx, region.size, &region.va);
      if (!region.address) {
        if (region.size > kWlfwChunkLimit) {
          c.memDelayed = true;   // too big for the window: ask the firmware to come back with smaller pieces
        } else {
          return fail(c, kWlfwErrMemory), false;
        }
      } else {
        c.regionGiven += region.size;
      }
    }
    c.regions[c.regionCount++] = region;
  }
  return true;
}

}  // namespace wlfwdetail

// One QMI message from the firmware's service port. Returns the step the client is in afterwards; kWlfwStepStopped means it ended
// (`error` says why). Anything that does not belong to the step it is in is counted and ignored, never acted on.
namespace wlfwdetail {
// The owner's log line for one message (WlfwIo::event).
inline void tell(WlfwClient &c, const QmiMessage &m, uint8_t kind)
{
  if (!c.io.event) return;
  WlfwEvent e;
  e.step = c.step;
  e.kind = kind;
  e.message = m.message;
  e.transaction = m.transaction;
  e.hasResult = qmiResult(m, &e.result, &e.error);
  if (kind == kWlfwEventResponse && c.outSegment >= 0) {
    e.segment = c.outSegment;
    e.segments = c.outSegments;
    e.bytes = c.outBytes;
    e.last = c.outLast;
    e.file = c.outFile;
  }
  c.io.event(c.io.ctx, e);
}
}  // namespace wlfwdetail

inline int wlfwReceive(WlfwClient &c, const uint8_t *data, uint32_t length)
{
  using namespace wlfwdetail;
  QmiMessage m;
  if (c.step == kWlfwStepStopped || c.step == kWlfwStepDone) return c.step;
  if (!qmiParse(data, length, m)) {
    fail(c, kWlfwErrMessage);
    return c.step;
  }
  if (m.type == kQmiIndication) {
    ++c.indications;
    const bool known = m.message == kWlfwRequestMemInd || m.message == kWlfwFwMemReadyInd || m.message == kWlfwFwReadyInd;
    const bool early = (m.message == kWlfwRequestMemInd && c.step < kWlfwStepHostCap) ||
                       (m.message == kWlfwFwMemReadyInd && c.step < kWlfwStepRespondMem) ||
                       (m.message == kWlfwFwReadyInd && c.step < kWlfwStepChipCap);
    tell(c, m, known && !early ? kWlfwEventIndication : kWlfwEventUnexpected);
    switch (m.message) {
    case kWlfwRequestMemInd:
      // Only after this host has said what it is (the firmware asks for memory in answer to that); the regions are read and
      // answered out of the window at once, since a later request replaces an earlier one.
      if (c.step < kWlfwStepHostCap) {
        ++c.unexpected;
        return c.step;
      }
      if (!readMemRequest(c, m)) return c.step;
      c.pendingMemRequest = true;
      if (c.step == kWlfwStepMemRequest) wlfwEnter(c, kWlfwStepMemRequest);
      return c.step;
    case kWlfwFwMemReadyInd:
      if (c.step < kWlfwStepRespondMem) {
        ++c.unexpected;
        return c.step;
      }
      c.pendingMemReady = true;
      if (c.step == kWlfwStepMemReady) wlfwEnter(c, kWlfwStepMemReady);
      return c.step;
    case kWlfwFwReadyInd:
      if (c.step < kWlfwStepChipCap) {   // before the chip's capability nothing of the firmware's own start-up has happened
        ++c.unexpected;
        return c.step;
      }
      c.pendingFwReady = true;
      if (c.step == kWlfwStepFwReady) wlfwEnter(c, kWlfwStepFwReady);
      return c.step;
    default:
      ++c.unexpected;
      return c.step;
    }
  }
  if (m.type != kQmiResponse || !c.waiting || m.transaction != c.pending || m.message != c.pendingMessage) {
    ++c.unexpected;
    tell(c, m, kWlfwEventUnexpected);
    return c.step;
  }
  ++c.responses;
  c.waiting = false;
  tell(c, m, kWlfwEventResponse);
  switch (c.step) {
  case kWlfwStepPhyCap:
    // Optional: a firmware that refuses it, or answers without a result, is not a failure (two links are then assumed, as this
    // chip's own driver does), so this one step does not go through resultOk.
    if (qmiResult(m, &c.result, &c.resultError) && c.result == kQmiResultSuccess) readPhyCap(c, m);
    wlfwEnter(c, kWlfwStepIndRegister);
    return c.step;
  case kWlfwStepIndRegister:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    qmiReadU64(m, 0x10, &c.fwStatus);
    wlfwEnter(c, kWlfwStepHostCap);
    return c.step;
  case kWlfwStepHostCap:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    wlfwEnter(c, kWlfwStepMemRequest);
    return c.step;
  case kWlfwStepRespondMem:
    if (!resultOk(c, m)) {
      // The empty form is refused on purpose: the firmware comes back with smaller regions.
      if (!c.memDelayed) return fail(c, kWlfwErrResult), c.step;
      c.memDelayed = false;
      wlfwEnter(c, kWlfwStepMemRequest);
      return c.step;
    }
    wlfwEnter(c, kWlfwStepMemReady);
    return c.step;
  case kWlfwStepChipCap:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    readChipCap(c, m);
    wlfwEnter(c, kWlfwStepRegdb);
    return c.step;
  case kWlfwStepQdss:
    if (qmiResult(m, &c.result, &c.resultError) && c.result != kQmiResultSuccess) {
      // A refused trace configuration is noted and the rest of the file is not sent: the firmware starts without it.
      c.qdssRefused = true;
      c.qdssResult = c.result;
      c.qdssError = c.resultError;
      wlfwEnter(c, kWlfwStepFwReady);
      return c.step;
    }
    [[fallthrough]];
  case kWlfwStepRegdb:
  case kWlfwStepBoard:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    if (c.fileOffset < c.fileBytes) {        // the same step: the next segment
      download(c);
      return c.step;
    }
    wlfwEnter(c, c.step == kWlfwStepRegdb ? kWlfwStepBoard : c.step == kWlfwStepBoard ? kWlfwStepM3 : kWlfwStepFwReady);
    return c.step;
  case kWlfwStepM3:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    wlfwEnter(c, kWlfwStepAuxUc);
    return c.step;
  case kWlfwStepAuxUc:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    wlfwEnter(c, kWlfwStepQdss);
    return c.step;
  case kWlfwStepIni:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    wlfwEnter(c, kWlfwStepCfg);
    return c.step;
  case kWlfwStepCfg:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    wlfwEnter(c, kWlfwStepMode);
    return c.step;
  case kWlfwStepMode:
    if (!resultOk(c, m)) return fail(c, kWlfwErrResult), c.step;
    wlfwEnter(c, kWlfwStepDone);
    return c.step;
  default:
    ++c.unexpected;
    return c.step;
  }
}

// The owner's deadline for the step the client stands in.
inline int wlfwTimedOut(WlfwClient &c)
{
  if (c.step == kWlfwStepDone || c.step == kWlfwStepStopped) return c.step;
  c.timedOutWaiting = c.waiting;         // the request outstanding stays in pending/pendingMessage/out* for the log
  wlfwdetail::fail(c, kWlfwErrTimeout);
  return c.step;
}

// The owner's deadline passed during the trace configuration: that step is given up (noted) and the client waits for the
// firmware's ready indication instead, since the trace is not part of the firmware's start. Returns false in any other step (the
// owner then times the client out as usual).
inline bool wlfwQdssGiveUp(WlfwClient &c)
{
  if (c.step != kWlfwStepQdss) return false;
  c.qdssTimedOut = true;
  c.waiting = false;
  wlfwEnter(c, kWlfwStepFwReady);
  return true;
}

inline bool wlfwFinished(const WlfwClient &c) { return c.step == kWlfwStepDone; }

}  // namespace pianowifi
