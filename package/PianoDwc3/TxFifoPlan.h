// SPDX-License-Identifier: BSD-2-Clause-Patent
// The DWC3's TX FIFO layout for the IN endpoints PianoDwc3 uses with the network function (piano-usb-net=1). IN endpoint n sends
// from TX FIFO n (DEPCFG's FIFO number); GTXFIFOSIZn holds its start (bits 31:16) and depth (DWC_usb3 15:0, DWC_usb31/32 14:0;
// bit 15 there is TXFRAMNUM), in MDWIDTH-wide words of one TX FIFO RAM (RAM1, or RAM0 shared with the RX FIFO when the core has a
// single-port RAM). A FIFO holding k packets of 512 bytes needs k * (512 / word + 1) + 1 words. The firmware's layout and the
// reset values are whatever coreConsultant chose; this plan only ever writes the FIFOs of endpoints in use, FIFO 0 (EP0) stays.
// Facts from the controller's programming model as mainline Linux drivers/usb/dwc3 (core.h, gadget.c: TX FIFO resizing)
// describes it, read as the specification only; the piano numbers (RAM1 3462 words of 8 bytes, 16 IN endpoints) from the T1
// probe (docs/drivers/usb-device-debug.md section 14). Portable: the kext and tests/ncm_host.cpp.
#pragma once
#include <stdint.h>

namespace pianousb {

struct TxFifoPlan {
  static constexpr unsigned kMax = 16;
  uint32_t value[kMax] = {};   // GTXFIFOSIZn to write; 0: left as it is (FIFO 0 always, and FIFOs of unused endpoints)
  unsigned fifos = 0;          // highest planned FIFO + 1
  uint32_t begin = 0, end = 0, ram = 0, wordBytes = 0;   // words: after FIFO 0, after the last planned FIFO, the RAM's depth
  const char *error = nullptr;
};

// MDWIDTH in bytes: GHWPARAMS0 bits 15:8 (bits), DWC_usb32 adds GHWPARAMS6 bits 9:8 as bits 9:8 of the width.
inline unsigned txFifoWordBytes(uint32_t hwparams0, uint32_t hwparams6, bool dwc32)
{
  const unsigned bits = ((hwparams0 >> 8) & 0xffu) | (dwc32 ? (hwparams6 & 0x300u) : 0u);
  return bits / 8;
}
// The TX FIFO RAM's depth in words: GHWPARAMS1 bit 23 set (single-port RAM): RAM0 (GHWPARAMS6 31:16), else RAM1 (GHWPARAMS7 15:0).
inline uint32_t txFifoRamWords(uint32_t hwparams1, uint32_t hwparams6, uint32_t hwparams7)
{
  return (hwparams1 >> 23) & 1u ? hwparams6 >> 16 : hwparams7 & 0xffffu;
}

// packets[n]: how many 512-byte packets FIFO n should hold (0: endpoint n IN unused); fifo0: GTXFIFOSIZ0 as it is.
inline TxFifoPlan planTxFifos(const uint8_t *packets, unsigned count, uint32_t fifo0, uint32_t ramWords, unsigned wordBytes, bool dwc31)
{
  TxFifoPlan p;
  p.ram = ramWords;
  p.wordBytes = wordBytes;
  const uint32_t depthMask = dwc31 ? 0x7fffu : 0xffffu, depth0 = fifo0 & depthMask;
  if (wordBytes < 4 || wordBytes > 64 || (wordBytes & (wordBytes - 1))) { p.error = "MDWIDTH not 32..512 bits"; return p; }
  if (!depth0) { p.error = "FIFO 0 has no depth"; return p; }
  if (count > TxFifoPlan::kMax) count = TxFifoPlan::kMax;
  const uint32_t perPacket = (512 + wordBytes - 1) / wordBytes + 1;   // the packet and its header word
  uint32_t at = (fifo0 >> 16) + depth0;
  p.begin = at;
  for (unsigned n = 1; n < count; ++n) {
    if (!packets[n]) continue;
    const uint32_t depth = packets[n] * perPacket + 1;
    if (depth > depthMask || at + depth > ramWords || at > 0xffffu) { p.error = "the FIFOs do not fit in the TX FIFO RAM"; return p; }
    p.value[n] = at << 16 | depth;
    at += depth;
    p.fifos = n + 1;
  }
  p.end = at;
  return p;
}

}  // namespace pianousb
