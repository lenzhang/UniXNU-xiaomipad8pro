// SPDX-License-Identifier: BSD-2-Clause-Patent
// The two 64-bit register accesses a DWC3 xHCI needs, as the only behaviour PianoXhciHost adds to Apple's generic xHCI driver
// (docs/drivers/usb-host.md §5.4, path A2). Templated on a register file so that the same code runs in the kext and in the host
// test (tests/reg64_host.cpp): REGS is anything with
//
//   uint32_t load(uint64_t offset);  void store(uint64_t offset, uint32_t value);  void barrier();
//
// Why: every DWC3 host carries Linux's `write-64-hi-lo-quirk` (xhci-plat.c / host.c; Project-SunUEFI saw the same order in
// Xiaomi's stock kernel): a 64-bit xHCI register is two 32-bit accesses and the **high half must be stored first**, so that the
// controller latches the whole address when the low half arrives. The base class of this collection reaches an 8-byte read helper
// for the 64-bit read, but only a 4-byte write helper for the 64-bit write (private/host-checks/20261114-xhci/REPORT.md §1), so
// the write override is what makes rings above 4 GiB work at all, and the read override makes the pair symmetric (the base read
// would be one 8-byte access, which this controller does not promise to answer as one).
//
// The read order is low half then high half, as Linux's xhci_read_64 does for the quirk, and as our own level-5 probe
// (package/PianoUsbHost/XhciHost.h rd64) does.
#pragma once
#include <stdint.h>

namespace pianoxhci {

// A 64-bit register read as two 32-bit loads, low half first.
template <class Regs> uint64_t read64(Regs &r, uint64_t offset)
{
  const uint32_t low = r.load(offset);
  const uint32_t high = r.load(offset + 4);
  return uint64_t(low) | uint64_t(high) << 32;
}

// A 64-bit register write as two 32-bit stores, the high half first, each followed by a barrier so the controller cannot see them
// in the other order (Linux's write-64-hi-lo-quirk).
template <class Regs> void write64(Regs &r, uint64_t offset, uint64_t value)
{
  r.store(offset + 4, uint32_t(value >> 32));
  r.barrier();
  r.store(offset, uint32_t(value));
  r.barrier();
}

}   // namespace pianoxhci
