// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoVideo V3 (piano-video=4), the host-firmware interface: the queue table and its three queues in the UC region, packets of
// HFI gen2, the boot words and the system handshake. Portable (host tests in tests/bringup_host.cpp against a firmware model that
// reads the command queue through our SMMU table and answers in the message and debug queues).
//
// The specification (GPL-2.0, read only; nothing copied): iris_hfi_queue.h/.c (the table and queue headers, the ring buffers in
// 32-bit words, iris_hfi_queues_init), iris_hfi_gen2_packet.h/.c (the 32-byte header and packets; sys_init with HFI_VIDEO_ARCH_LX
// and the seven UBWC properties of ubwc_config.c's SM8750 entry, image_version as a get-property, interframe power collapse, pc_prep),
// iris_hfi_gen2_response.c (validate_hdr_packet; a system response: system error by flag or type range, INIT success, the version
// string of IRIS_FW_VERSION_LENGTH with NULs as spaces; the debug queue), iris_vpu_common.c (iris_vpu_setup_ucregion_memory_map,
// iris_vpu35_vpu4x_program_bootup_registers, iris_vpu_boot_firmware, iris_vpu_raise_interrupt).
//
// Memory: the UC region is kUcBytes of XNU pages, seen by the CPU through a write-combined mapping (Normal non-cacheable) and by the
// core through our SMMU bank as Normal non-cacheable: no cache maintenance; a barrier orders a packet before its write index, and
// the write index before the doorbell.
#pragma once
#include <stdint.h>
#include "VideoHw.h"

namespace pianovideo {
namespace hfi {

// --------------------------------------------------------------------------------------------------------- the queue table
constexpr uint32_t kTblVersion = 0, kTblSize = 4, kTblQhdr0Offset = 8, kTblQhdrSize = 12, kTblNumQ = 16, kTblNumActiveQ = 20,
                   kTblDeviceAddr = 24, kTblName = 32, kTblQhdr = 288, kQhdrBytes = 56, kNumQ = 3;
static_assert(kTblQhdr + kNumQ * kQhdrBytes == kQueueTableBytes, "456 bytes");
constexpr uint32_t kQStatus = 0, kQStart = 4, kQType = 8, kQSize = 12, kQPktSize = 16, kQDrop = 20, kQRxWm = 24, kQTxWm = 28,
                   kQRxReq = 32, kQTxReq = 36, kQRxIrq = 40, kQTxIrq = 44, kQRead = 48, kQWrite = 52;
enum Queue : unsigned { kCmdq = 0, kMsgq = 1, kDbgq = 2 };
constexpr uint32_t qhdr(unsigned q) { return kTblQhdr + kQhdrBytes * q; }
constexpr uint32_t qbase(unsigned q) { return kQueueTableBytes + q * kHfiQueueBytes; }
static_assert(qbase(3) <= kSfrOffset, "the SFR after the queues");

struct Mem {
  volatile uint32_t *uc = nullptr;   // kUcBytes, the CPU's write-combined view
  uint32_t iova = kIovaBase;         // the core's view of byte 0
  uint32_t ld(uint32_t off) const { return uc[off / 4]; }
  void st(uint32_t off, uint32_t v) { uc[off / 4] = v; }
};

// iris_hfi_queues_init: the table header, the three queue headers, the SFR's size word. The memory was zeroed by its owner.
inline void initQueues(Mem &m)
{
  m.st(kTblVersion, 0);
  m.st(kTblSize, kQueueTableBytes);
  m.st(kTblQhdr0Offset, kTblQhdr);
  m.st(kTblQhdrSize, kQhdrBytes);
  m.st(kTblNumQ, kNumQ);
  m.st(kTblNumActiveQ, kNumQ);
  m.st(kTblDeviceAddr, 0);   // Linux stores its core pointer here (a cookie the firmware does not use); ours is 0
  m.st(kTblDeviceAddr + 4, 0);
  const char name[] = "iris-hfi-queues";
  for (uint32_t i = 0; i < 256; i += 4) {
    uint32_t w = 0;
    for (uint32_t b = 0; b < 4; ++b) w |= uint32_t(i + b < sizeof name ? uint8_t(name[i + b]) : 0) << (8 * b);
    m.st(kTblName + i, w);
  }
  for (unsigned q = 0; q < kNumQ; ++q) {
    const uint32_t h = qhdr(q);
    m.st(h + kQStatus, 1);
    m.st(h + kQStart, m.iova + qbase(q));
    m.st(h + kQType, q | kIfaceqDefaultHeader << 16);   // u16 queue_type, u16 header_type
    m.st(h + kQSize, kHfiQueueBytes / 4);
    m.st(h + kQPktSize, 0);                              // variable-size packets
    m.st(h + kQDrop, 0);
    m.st(h + kQRxWm, 1);
    m.st(h + kQTxWm, 1);
    m.st(h + kQRxReq, q == kDbgq ? 0 : 1);
    m.st(h + kQTxReq, 0);
    m.st(h + kQRxIrq, 0);
    m.st(h + kQTxIrq, 0);
    m.st(h + kQRead, 0);
    m.st(h + kQWrite, 0);
  }
  m.st(kSfrOffset, kSfrBytes);
}

// iris_hfi_queue_write: bytes (a multiple of 4) at the write index, wrapping at the queue's end; the index after a barrier.
// 0, or -1 when the queue has no room (one word is always kept free, so a full queue is never mistaken for an empty one).
template <class Barrier>
int write(Mem &m, unsigned q, const uint32_t *packet, uint32_t bytes, Barrier barrier)
{
  const uint32_t h = qhdr(q), w = m.ld(h + kQWrite) * 4, r = m.ld(h + kQRead) * 4;
  if ((bytes & 3) || !bytes || w >= kHfiQueueBytes || r >= kHfiQueueBytes) return -2;
  const uint32_t room = w < r ? r - w : kHfiQueueBytes - (w - r);
  if (room <= bytes) return -1;
  m.st(h + kQTxReq, 0);
  uint32_t at = w;
  for (uint32_t i = 0; i < bytes / 4; ++i) {
    m.st(qbase(q) + at, packet[i]);
    at = (at + 4) % kHfiQueueBytes;
  }
  barrier();
  m.st(h + kQWrite, at / 4);
  barrier();
  return 0;
}

// iris_hfi_queue_read: one packet (its first word is its size in bytes) into out. 1 when the queue is empty, 0 with *bytes set, -1
// for a zero size, -2 for a packet larger than IFACEQ_CORE_PKT_SIZE (dropped: the read index jumps to the write index), -3 for
// indices outside the queue.
template <class Barrier>
int read(Mem &m, unsigned q, uint32_t *out, uint32_t cap, uint32_t *bytes, Barrier barrier)
{
  const uint32_t h = qhdr(q), w = m.ld(h + kQWrite) * 4, r = m.ld(h + kQRead) * 4, rxReq = q == kMsgq ? 1 : 0;
  *bytes = 0;
  if (w >= kHfiQueueBytes || r >= kHfiQueueBytes) return -3;
  if (r == w) {
    m.st(h + kQRxReq, rxReq);
    barrier();
    return 1;
  }
  barrier();   // the packet after the index that announced it
  const uint32_t size = m.ld(qbase(q) + r);
  if (!size) return -1;
  int rc = 0;
  uint32_t next = w;
  if (size <= kHfiCorePacketBytes && size <= cap && !(size & 3)) {
    uint32_t at = r;
    for (uint32_t i = 0; i < size / 4; ++i) {
      out[i] = m.ld(qbase(q) + at);
      at = (at + 4) % kHfiQueueBytes;
    }
    next = at;
    *bytes = size;
  } else {
    rc = -2;
  }
  m.st(h + kQRxReq, rxReq);
  m.st(h + kQRead, next / 4);
  barrier();
  return rc;
}

// ------------------------------------------------------------------------------------------------------------------ packets
struct Ids {
  uint32_t header = 0, packet = 0;   // iris_core's header_id / packet_id: from 0, one more per header and per packet
};
// The header: {size, session_id, header_id, reserved[4], num_packets}; returns its size.
inline uint32_t begin(uint32_t *buf, uint32_t session, Ids &ids)
{
  for (unsigned i = 0; i < 8; ++i) buf[i] = 0;
  buf[0] = kHfiHeaderBytes;
  buf[1] = session;
  buf[2] = ids.header++;
  return kHfiHeaderBytes;
}
// One packet {size, type, flags, payload_info, port, packet_id, reserved[2]} + payload words, appended; the header's size and count.
inline void add(uint32_t *buf, uint32_t type, uint32_t flags, uint32_t payloadInfo, const uint32_t *payload, uint32_t words, Ids &ids)
{
  uint32_t *p = buf + buf[0] / 4;
  const uint32_t size = kHfiPacketBytes + 4 * words;
  p[0] = size;
  p[1] = type;
  p[2] = flags;
  p[3] = payloadInfo;
  p[4] = kHfiPortNone;
  p[5] = ids.packet++;
  p[6] = p[7] = 0;
  for (uint32_t i = 0; i < words; ++i) p[8 + i] = payload[i];
  buf[0] += size;
  buf[7] += 1;
}
constexpr uint32_t kSysInitBytes = kHfiHeaderBytes + 8 * (kHfiPacketBytes + 4);   // 320
constexpr uint32_t kBufferWords = kHfiCorePacketBytes / 4;
inline uint32_t sysInit(uint32_t *buf, Ids &ids)
{
  begin(buf, 0, ids);
  const uint32_t arch = kHfiArchLx;
  add(buf, kHfiCmdInit, kHfiHostResponse | kHfiHostIntr | kHfiHostNonDiscardable, kHfiPayloadU32, &arch, 1, ids);
  const struct {
    uint32_t type, value;
  } props[] = {{kHfiPropUbwcMaxChannels, kUbwcMaxChannels}, {kHfiPropUbwcMalLength, kUbwcMalLength}, {kHfiPropUbwcHbb, kUbwcHbb},
               {kHfiPropUbwcSwzl1, kUbwcSwzl1},             {kHfiPropUbwcSwzl2, kUbwcSwzl2},         {kHfiPropUbwcSwzl3, kUbwcSwzl3},
               {kHfiPropUbwcBankSpreading, kUbwcBankSpreading}};
  for (const auto &p : props) add(buf, p.type, 0, kHfiPayloadU32, &p.value, 1, ids);
  return buf[0];
}
inline uint32_t imageVersion(uint32_t *buf, Ids &ids)
{
  begin(buf, 0, ids);
  add(buf, kHfiPropImageVersion, kHfiHostResponse | kHfiHostIntr | kHfiHostGetProperty, kHfiPayloadNone, nullptr, 0, ids);
  return buf[0];
}
inline uint32_t interframePowerCollapse(uint32_t *buf, Ids &ids)
{
  begin(buf, 0, ids);
  const uint32_t on = 1;
  add(buf, kHfiPropIntraFramePc, 0, kHfiPayloadU32, &on, 1, ids);
  return buf[0];
}
inline uint32_t pcPrep(uint32_t *buf, Ids &ids)
{
  begin(buf, 0, ids);
  add(buf, kHfiCmdPowerCollapse, 0, kHfiPayloadNone, nullptr, 0, ids);
  return buf[0];
}

// ---------------------------------------------------------------------------------------------------------------- answers
struct Reply {
  unsigned headers = 0, packets = 0, sessionHeaders = 0, malformed = 0;
  bool initDone = false, initFailed = false, version = false, sysError = false;
  uint32_t errorType = 0, errorFlags = 0;
  char text[kHfiVersionChars] = {};   // the firmware's version string (NULs as spaces), NUL-terminated
};
// iris_hfi_gen2_handle_response for one message: false when malformed (Linux then raises a system error).
inline bool parse(const uint32_t *buf, uint32_t bytes, Reply &r)
{
  ++r.headers;
  if (bytes < kHfiHeaderBytes + kHfiPacketBytes || buf[0] != bytes || buf[0] < kHfiHeaderBytes + kHfiPacketBytes) {
    ++r.malformed;
    return false;
  }
  const uint32_t count = buf[7];
  uint32_t at = kHfiHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {   // validate_hdr_packet: every packet within the 4 KiB response buffer and the message
    if (at + kHfiPacketBytes > bytes) {
      ++r.malformed;
      return false;
    }
    const uint32_t size = buf[at / 4];
    if (size < kHfiPacketBytes || (size & 3) || at + size > bytes) {
      ++r.malformed;
      return false;
    }
    at += size;
  }
  if (buf[1]) {   // a session's message: none can come before a session is opened
    ++r.sessionHeaders;
    return true;
  }
  at = kHfiHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t *p = buf + at / 4, size = p[0], type = p[1], flags = p[2];
    ++r.packets;
    if ((flags & kHfiFwSystemError) || (type > kHfiSysErrorBegin && type < kHfiSysErrorEnd)) {
      r.sysError = true;
      r.errorType = type;
      r.errorFlags = flags;
      return true;
    }
    if (type == kHfiPropImageVersion) {
      const uint8_t *s = reinterpret_cast<const uint8_t *>(p + 8);
      const uint32_t have = size - kHfiPacketBytes;
      if (have >= kHfiVersionChars - 1) {
        for (uint32_t c = 0; c < kHfiVersionChars - 1; ++c) r.text[c] = s[c] ? char(s[c]) : ' ';
        r.text[kHfiVersionChars - 1] = 0;
        r.version = true;
      } else {
        ++r.malformed;
      }
    } else if (type > kHfiCmdBegin && type < kHfiCmdEnd) {
      if (flags & kHfiFwSuccess) r.initDone = true;
      else r.initFailed = true;
    }
    at += size;
  }
  return true;
}
// A debug queue message: {size, debug_level, reserved[2]} then the text from byte 17 (iris_hfi_gen2_flush_debug_queue), copied
// printable-only into line (at most cap - 1 characters). False when the message is too short.
inline bool debugText(const uint32_t *buf, uint32_t bytes, char *line, uint32_t cap)
{
  if (bytes < 16 || buf[0] < 16 || buf[0] > bytes || !cap) return false;
  const uint8_t *s = reinterpret_cast<const uint8_t *>(buf);
  uint32_t n = 0;
  for (uint32_t i = 17; i < buf[0] && n + 1 < cap; ++i) {
    const uint8_t c = s[i];
    if (!c) break;
    if (c == '\n' || c == '\r') continue;
    line[n++] = c >= 0x20 && c < 0x7f ? char(c) : '?';
  }
  line[n] = 0;
  return true;
}
// The SFR (subsystem failure reason): its size word, then text.
inline void sfrText(const Mem &m, char *line, uint32_t cap)
{
  uint32_t n = 0;
  for (uint32_t i = 4; i < kSfrBytes && n + 1 < cap; ++i) {
    const uint8_t c = uint8_t(m.ld(kSfrOffset + (i & ~3u)) >> (8 * (i & 3)));
    if (!c) break;
    line[n++] = c >= 0x20 && c < 0x7f ? char(c) : '?';
  }
  line[n] = 0;
}

// --------------------------------------------------------------------------------------------------------- the boot words
// iris_vpu_setup_ucregion_memory_map + iris_vpu35_vpu4x_program_bootup_registers + iris_vpu_boot_firmware. 0, kErrBoot with
// *status = CTRL_STATUS (an error code, or 0 after 1000 polls).
template <class O>
int boot(O &ops, uint32_t iova, uint32_t *status)
{
  ops.wr(kIris + kUcRegionAddr, iova, "UC_REGION_ADDR: the queue table");
  ops.wr(kIris + kUcRegionSize, kUcRegionBytes, "UC_REGION_SIZE: 4 MiB");
  ops.wr(kIris + kQtblAddr, iova, "QTBL_ADDR");
  ops.wr(kIris + kQtblInfo, 1, "QTBL_INFO: enabled");
  ops.wr(kIris + kSfrAddr, iova + kSfrOffset + kSfrCoreArch, "SFR_ADDR: the SFR + VIDEO_ARCH_LX");
  ops.wr(kIris + kWrapperSpare0, 1, "WRAPPER_SPARE_0: 1 (VPU 3.5 boot)");
  ops.wr(kIris + kCtrlInit, 1, "CTRL_INIT: start the HFI");
  ops.wr(kIris + kSciaCmdArg3, 1, "SCIACMDARG3: 1");
  uint32_t v = 0;
  const bool answered = ops.pollFor(kIris + kCtrlStatus, [](uint32_t s) { return s != 0; }, kBootPolls * kBootPollUs, kBootPollUs, &v);
  *status = v;
  if (!answered || (v & kCtrlStatusErrorMask) || !(v & kCtrlStatusInitDone)) return kErrBoot;
  ops.wr(kIris + kH2xSoftIntEn, kH2xEnable, "H2XSOFTINTEN: host-to-firmware interrupt on");
  ops.wr(kIris + kX2Rpmh, 0, "X2RPMH: unmasked");
  return kOk;
}

}  // namespace hfi
}  // namespace pianovideo
