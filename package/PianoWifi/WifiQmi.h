// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 5: the QMI message codec the WLFW client writes and reads (host tests: tests/qmi_host.cpp). New code for this
// repository; the wire format is a fact of the protocol, the same one PianoPower's copied QRTR codec parses on the DSP link:
//
//   a message is a 7-byte header — type (1: request 0, response 2, indication 4), transaction (2, little endian), message id (2),
//   the payload's length (2) — followed by TLVs: type (1), length (2, little endian), value.
//   Inside a TLV, scalars are little endian and structures are their members in order with no padding at all; an optional member
//   is simply a TLV that is absent (what the encoders call a "valid" flag is the sender's bookkeeping, never a wire byte).
//   A variable-length array inside a TLV begins with its element count, one byte when the maximum count fits a byte and two bytes
//   otherwise (the service's own description of the member decides which, so each message's builder says so here);
//   a string TLV of its own carries only its characters: no count in front and no terminating zero.
//   A response carries TLV 0x02: result (2) then error (2); result 0 is success.
//
// The writer never writes past the buffer it was given: an overflow is remembered, every later write is dropped, and the message is
// refused as a whole (qmiEnd returns 0). That is on purpose — the board file's download messages are a few bytes short of 6 KiB and
// are built straight in the transport's buffer.
#pragma once
#include <stdint.h>
#include "WifiQrtr.h"

namespace pianowifi {

constexpr uint32_t kQmiHeaderBytes = 7, kQmiTlvHeaderBytes = 3;
enum QmiMsgType : uint8_t { kQmiRequest = 0, kQmiResponse = 2, kQmiIndication = 4 };
constexpr uint8_t kQmiTlvResult = 0x02;
constexpr uint16_t kQmiResultSuccess = 0;

// ------------------------------------------------------------------------------------------------------------- the writer
struct QmiWriter {
  uint8_t *buffer = nullptr;
  uint32_t capacity = 0, length = 0;
  uint32_t open = 0;        // byte offset of the open TLV's length field, 0 when none is open
  bool overflow = false;
};

inline void qmiBegin(QmiWriter &w, uint8_t *buffer, uint32_t capacity, uint8_t type, uint16_t transaction, uint16_t message)
{
  w = QmiWriter{buffer, capacity, 0, 0, false};
  if (!buffer || capacity < kQmiHeaderBytes) {
    w.overflow = true;
    return;
  }
  buffer[0] = type;
  buffer[1] = uint8_t(transaction);
  buffer[2] = uint8_t(transaction >> 8);
  buffer[3] = uint8_t(message);
  buffer[4] = uint8_t(message >> 8);
  buffer[5] = 0;
  buffer[6] = 0;
  w.length = kQmiHeaderBytes;
}

// Raw bytes into the open TLV (or nothing at all when the message has already overflowed).
inline void qmiRaw(QmiWriter &w, const void *from, uint32_t bytes)
{
  if (w.overflow) return;
  if (!w.open || bytes > w.capacity - w.length) {
    w.overflow = true;
    return;
  }
  const uint8_t *p = static_cast<const uint8_t *>(from);
  for (uint32_t i = 0; i < bytes; ++i) w.buffer[w.length + i] = p[i];
  w.length += bytes;
}

inline void qmiLe(QmiWriter &w, uint64_t value, unsigned bytes)
{
  if (w.overflow) return;
  if (!w.open || bytes > w.capacity - w.length) {
    w.overflow = true;
    return;
  }
  for (unsigned i = 0; i < bytes; ++i) w.buffer[w.length + i] = uint8_t(value >> (8 * i));
  w.length += bytes;
}

inline void qmiTlvBegin(QmiWriter &w, uint8_t type)
{
  if (w.overflow) return;
  if (w.open || kQmiTlvHeaderBytes > w.capacity - w.length) {
    w.overflow = true;
    return;
  }
  w.buffer[w.length] = type;
  w.buffer[w.length + 1] = 0;
  w.buffer[w.length + 2] = 0;
  w.open = w.length + 1;
  w.length += kQmiTlvHeaderBytes;
}

inline void qmiTlvEnd(QmiWriter &w)
{
  if (w.overflow || !w.open) return;
  const uint32_t bytes = w.length - (w.open + 2);
  if (bytes > 0xffffu) {
    w.overflow = true;
    return;
  }
  w.buffer[w.open] = uint8_t(bytes);
  w.buffer[w.open + 1] = uint8_t(bytes >> 8);
  w.open = 0;
}

inline void qmiU8(QmiWriter &w, uint8_t type, uint8_t value)
{
  qmiTlvBegin(w, type);
  qmiLe(w, value, 1);
  qmiTlvEnd(w);
}
inline void qmiU16(QmiWriter &w, uint8_t type, uint16_t value)
{
  qmiTlvBegin(w, type);
  qmiLe(w, value, 2);
  qmiTlvEnd(w);
}
inline void qmiU32(QmiWriter &w, uint8_t type, uint32_t value)
{
  qmiTlvBegin(w, type);
  qmiLe(w, value, 4);
  qmiTlvEnd(w);
}
inline void qmiU64(QmiWriter &w, uint8_t type, uint64_t value)
{
  qmiTlvBegin(w, type);
  qmiLe(w, value, 8);
  qmiTlvEnd(w);
}
// A string TLV of its own: the characters, no count, no terminating zero (`limit` bounds what is copied).
inline void qmiString(QmiWriter &w, uint8_t type, const char *text, uint32_t limit)
{
  qmiTlvBegin(w, type);
  for (uint32_t i = 0; i < limit && text && text[i]; ++i) qmiLe(w, uint8_t(text[i]), 1);
  qmiTlvEnd(w);
}
// A TLV holding bytes (the file segments of the board-file and configuration downloads).
inline void qmiBytes(QmiWriter &w, uint8_t type, const void *from, uint32_t bytes, unsigned countBytes)
{
  qmiTlvBegin(w, type);
  if (countBytes) qmiLe(w, bytes, countBytes);
  qmiRaw(w, from, bytes);
  qmiTlvEnd(w);
}

// Writes the payload length into the header; returns the whole message's length, or 0 when anything overflowed or a TLV was left
// open (a programming error the host tests catch).
inline uint32_t qmiEnd(QmiWriter &w)
{
  if (w.overflow || w.open || w.length < kQmiHeaderBytes) return 0;
  const uint32_t payload = w.length - kQmiHeaderBytes;
  if (payload > 0xffffu) return 0;
  w.buffer[5] = uint8_t(payload);
  w.buffer[6] = uint8_t(payload >> 8);
  return w.length;
}

// ------------------------------------------------------------------------------------------------------------- the reader
// Parsing is the copied codec's (WifiQrtr.h, from PianoPower): one header and a bounded TLV walk. These are the typed readers the
// WLFW client wants on top of it; each returns false when the TLV is absent or not the size the member has.
using QmiMessage = QcomQmiMessage;

inline bool qmiParse(const uint8_t *data, uint32_t length, QmiMessage &m)
{
  return qcom_qmi_parse(data, length, &m) == 0;
}
inline const uint8_t *qmiFind(const QmiMessage &m, uint8_t type, uint32_t *size)
{
  return qcom_qmi_tlv(&m, type, size);
}
inline bool qmiScalar(const QmiMessage &m, uint8_t type, unsigned bytes, uint64_t *out)
{
  uint32_t size = 0;
  const uint8_t *at = qmiFind(m, type, &size);
  if (!at || size != bytes) return false;
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; ++i) v |= uint64_t(at[i]) << (8 * i);
  if (out) *out = v;
  return true;
}
inline bool qmiReadU8(const QmiMessage &m, uint8_t type, uint8_t *out)
{
  uint64_t v = 0;
  if (!qmiScalar(m, type, 1, &v)) return false;
  if (out) *out = uint8_t(v);
  return true;
}
inline bool qmiReadU16(const QmiMessage &m, uint8_t type, uint16_t *out)
{
  uint64_t v = 0;
  if (!qmiScalar(m, type, 2, &v)) return false;
  if (out) *out = uint16_t(v);
  return true;
}
inline bool qmiReadU32(const QmiMessage &m, uint8_t type, uint32_t *out)
{
  uint64_t v = 0;
  if (!qmiScalar(m, type, 4, &v)) return false;
  if (out) *out = uint32_t(v);
  return true;
}
inline bool qmiReadU64(const QmiMessage &m, uint8_t type, uint64_t *out)
{
  uint64_t v = 0;
  if (!qmiScalar(m, type, 8, &v)) return false;
  if (out) *out = v;
  return true;
}
// A response's result TLV. False when it is absent or malformed, which is itself a failure.
inline bool qmiResult(const QmiMessage &m, uint16_t *result, uint16_t *error)
{
  uint32_t size = 0;
  const uint8_t *at = qmiFind(m, kQmiTlvResult, &size);
  if (!at || size != 4) return false;
  if (result) *result = uint16_t(at[0] | uint16_t(at[1]) << 8);
  if (error) *error = uint16_t(at[2] | uint16_t(at[3]) << 8);
  return true;
}

// A reader over the bytes of one TLV: the structure members in order, each bounds-checked.
struct QmiTlvReader {
  const uint8_t *at = nullptr;
  uint32_t size = 0, used = 0;
  bool bad = false;
};
inline QmiTlvReader qmiTlvReader(const QmiMessage &m, uint8_t type)
{
  QmiTlvReader r;
  r.at = qmiFind(m, type, &r.size);
  r.bad = r.at == nullptr;
  return r;
}
inline uint64_t qmiTake(QmiTlvReader &r, unsigned bytes)
{
  if (r.bad || bytes > r.size - r.used) {
    r.bad = true;
    return 0;
  }
  uint64_t v = 0;
  for (unsigned i = 0; i < bytes; ++i) v |= uint64_t(r.at[r.used + i]) << (8 * i);
  r.used += bytes;
  return v;
}

}  // namespace pianowifi
