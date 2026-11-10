// SPDX-License-Identifier: BSD-2-Clause-Patent
// The descriptors of the piano debug gadget: a composite device (IAD) with kPorts CDC-ACM functions, USB 2.0 high/full
// speed. Port 0 is the kernel console and monitor, port 1 the user pipe (/dev/pianousb). Layout from the USB 2.0 spec
// (chapter 9), the USB IAD ECN and CDC 1.2 / PSTN 1.2 (ACM); no other source. Portable: built for the kernel and for the
// host test (tests/usb_gadget_host.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pianousb {

constexpr unsigned kPorts = 2;
// pid.codes test VID/PID (1209:0001, "for private testing"); piano-usb-pid=<n> overrides the PID.
constexpr uint16_t kVendor = 0x1209, kProduct = 0x0001, kRelease = 0x0100;
// Fixed strings only: no unit serial (docs/drivers/README.md privacy rule).
constexpr const char *kStrings[] = {nullptr, "UniXNU", "piano macOS debug", "PIANODBG", "piano kernel console", "piano user pipe"};
constexpr unsigned kStringCount = sizeof kStrings / sizeof kStrings[0];

// Logical endpoints of port p: notification IN (1 + 2p), bulk IN/OUT (2 + 2p).
constexpr uint8_t notifyEp(unsigned p) { return uint8_t(1 + 2 * p); }
constexpr uint8_t bulkEp(unsigned p) { return uint8_t(2 + 2 * p); }
// DWC3 physical endpoint: 2 * number + direction (1 = IN)
constexpr unsigned physOf(uint8_t ep, bool in) { return ep * 2u + (in ? 1u : 0u); }
constexpr uint16_t kNotifyMps = 16;
constexpr uint8_t kNotifyIntervalHs = 9, kNotifyIntervalFs = 32;   // 2^(9-1) x 125 us = 32 ms; 32 ms

struct DescWriter {
  uint8_t *out;
  unsigned cap, len;
  void b(uint8_t v) { if (len < cap) out[len] = v; ++len; }
  void w(uint16_t v) { b(uint8_t(v)); b(uint8_t(v >> 8)); }
};

inline unsigned deviceDescriptor(uint8_t *out, unsigned cap, uint16_t product)
{
  DescWriter d{out, cap, 0};
  d.b(18); d.b(1); d.w(0x0200);
  d.b(0xef); d.b(0x02); d.b(0x01);           // miscellaneous / common class / interface association
  d.b(64);
  d.w(kVendor); d.w(product); d.w(kRelease);
  d.b(1); d.b(2); d.b(3); d.b(1);
  return d.len;
}

inline unsigned qualifierDescriptor(uint8_t *out, unsigned cap)
{
  DescWriter d{out, cap, 0};
  d.b(10); d.b(6); d.w(0x0200); d.b(0xef); d.b(0x02); d.b(0x01); d.b(64); d.b(1); d.b(0);
  return d.len;
}

// type 2 (configuration) or 7 (other speed configuration); highSpeed selects the packet sizes of the bulk endpoints;
// ports (1..kPorts) the number of ACM functions (1 when the core has too few IN endpoints, see PianoDwc3.cpp).
inline unsigned configDescriptor(uint8_t *out, unsigned cap, uint8_t type, bool highSpeed, unsigned ports = kPorts)
{
  const uint16_t mps = highSpeed ? 512 : 64;
  DescWriter d{out, cap, 0};
  d.b(9); d.b(type); d.w(0);                 // total length patched below
  d.b(uint8_t(2 * ports)); d.b(1); d.b(0); d.b(0xc0); d.b(50);   // self powered, 100 mA
  for (unsigned p = 0; p < ports; ++p) {
    const uint8_t comm = uint8_t(2 * p), data = uint8_t(2 * p + 1), name = uint8_t(4 + p);
    d.b(8); d.b(0x0b); d.b(comm); d.b(2); d.b(0x02); d.b(0x02); d.b(0x01); d.b(name);          // IAD
    d.b(9); d.b(4); d.b(comm); d.b(0); d.b(1); d.b(0x02); d.b(0x02); d.b(0x01); d.b(name);     // communications interface (ACM)
    d.b(5); d.b(0x24); d.b(0x00); d.w(0x0110);                                                 // header
    d.b(5); d.b(0x24); d.b(0x01); d.b(0x00); d.b(data);                                        // call management
    d.b(4); d.b(0x24); d.b(0x02); d.b(0x02);                                                   // ACM: line coding + serial state
    d.b(5); d.b(0x24); d.b(0x06); d.b(comm); d.b(data);                                        // union
    d.b(7); d.b(5); d.b(uint8_t(0x80 | notifyEp(p))); d.b(0x03); d.w(kNotifyMps);
    d.b(highSpeed ? kNotifyIntervalHs : kNotifyIntervalFs);
    d.b(9); d.b(4); d.b(data); d.b(0); d.b(2); d.b(0x0a); d.b(0x00); d.b(0x00); d.b(name);     // data interface (named too: the
                                                                                               // Mac's serial node hangs below it)
    d.b(7); d.b(5); d.b(bulkEp(p)); d.b(0x02); d.w(mps); d.b(0);                               // bulk OUT
    d.b(7); d.b(5); d.b(uint8_t(0x80 | bulkEp(p))); d.b(0x02); d.w(mps); d.b(0);               // bulk IN
  }
  if (cap >= 4) { out[2] = uint8_t(d.len); out[3] = uint8_t(d.len >> 8); }
  return d.len;
}

inline unsigned stringDescriptor(uint8_t *out, unsigned cap, unsigned index)
{
  DescWriter d{out, cap, 0};
  if (index == 0) { d.b(4); d.b(3); d.w(0x0409); return d.len; }
  if (index >= kStringCount) return 0;
  const char *s = kStrings[index];
  unsigned n = 0;
  while (s[n]) ++n;
  d.b(uint8_t(2 + 2 * n)); d.b(3);
  for (unsigned i = 0; i < n; ++i) d.w(uint8_t(s[i]));
  return d.len;
}

}  // namespace pianousb
