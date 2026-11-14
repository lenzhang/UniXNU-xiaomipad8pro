// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoXhciHost: the smallest subclass of this kernel collection's generic Apple xHCI driver that a Qualcomm DWC3 controller needs
// (path **A2** of docs/drivers/usb-host.md §5.4; the owner approved the binary analysis behind it on 2026-11-14). It adds no
// instance field and exactly three behavioural overrides of the 427 virtual slots:
//
//   slot 292  the desired/default power state: always on (3) for a bounded host session. Once the root ports exist, Apple's family
//             leaves the controller's power to them, and an idle port would let it fall to off; what wakes it on Apple's hardware
//             is the Type-C transport's cable report, which this provider does not have. Our role manager only creates this
//             controller while a peripheral is on the cable (docs/drivers/usb-host.md §6.1).
//   slot 419  the 64-bit register read: two 32-bit loads, low half first.
//   slot 421  the 64-bit register write: the high half first, then the low half, each followed by a barrier - Linux's
//             `write-64-hi-lo-quirk`, which every DWC3 host carries. The base class of this collection reaches a **4-byte** write
//             helper for 64-bit registers (private/host-checks/20261114-xhci/REPORT.md §1), so a ring above 4 GiB would be latched
//             with half an address without this override; the read/modify/write accessors dispatch through slots 419 and 421, so
//             they are fixed by the same two.
//
// Everything else - the register window, the interrupt, the DWC3 role switch, the PHY, VBUS, clocks, the SMMU route and the
// halted/reset controller Apple's start() finds - stays outside this class, in PianoUsbHost's nub and host switch. Apple's generic
// ARM layer (AppleUSBXHCIARM) and its SoC leaves are deliberately **not** the base class: they want AppleARMIODevice resources,
// Apple's Type-C PHY, mapper and clock sessions, and they do not override the 64-bit accessors anyway.
//
// The base-class declarations come from OriginalUsbXhciABI.h, generated at build time by tooling/k27/xhci-abi.py from
// data/usb-xhci/abi-26C5035r-mac17j.json (slot numbers, PAC diversities and names; no Apple code). The build refuses to link
// unless the collection is the one that data file was made from and the compiled vtable is slot for slot the original's with only
// these three entries ours (tooling/k27/build-native-piano-xhcihost.sh).
//
// Linked only with PIANO_USBHOST_A2=1 (tooling/k27/build-native-image.sh); it matches nothing until PianoUsbHost's level 6 makes
// the nub (`piano-usbhost=6 piano-usbhost-a2=1`), because its only personality's provider class is PianoUsbHostNub.
#include "OriginalUsbXhciABI.h"
#include "Reg64.h"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>

namespace {

// The register file behind Apple's 64-bit accessors: `base` is the mapped window Apple's driver passes, `offset` the register.
struct Window {
  volatile uint32_t *at;
  uint32_t load(uint64_t offset) { return at[offset / 4]; }
  void store(uint64_t offset, uint32_t value) { at[offset / 4] = value; }
  void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
};

// Evidence for the first tablet boot at no cost in the steady state: the first few accesses of each kind only. Those are the ones
// that matter (the rings are programmed once: DCBAAP, CRCR, ERSTBA, ERDP), and they happen in Apple's start() on its work loop,
// not in interrupt context; later accesses (an ERDP update per event batch) log nothing, so no IOLog can reach a primary
// interrupt handler. The totals go out in the module's stop line.
uint64_t gReads, gWrites;
constexpr uint64_t kLogFirst = 8;

bool logging(uint64_t count) { return count <= kLogFirst; }

}   // namespace

class PianoXhciHost : public AppleUSBXHCI {
  OSDeclareDefaultStructors(PianoXhciHost)

public:
  // Slot 292: the power state this controller asks for while it exists.
  uint32_t XHCI_DESIRED_POWER() override { return 3; }

  // Slot 419: a 64-bit register as two 32-bit loads, low half first.
  uint64_t XHCI_READ64(void *base, uint32_t offset) override
  {
    Window w = {static_cast<volatile uint32_t *>(base)};
    const uint64_t value = pianoxhci::read64(w, offset);
    const uint64_t count = ++gReads;
    if (logging(count))
      IOLog("PianoXhciHost: read64 #%llu +%#x = %#llx (low %#x, high %#x)\n", (unsigned long long)count, offset,
            (unsigned long long)value, uint32_t(value), uint32_t(value >> 32));
    return value;
  }

  // Slot 421: a 64-bit register as two 32-bit stores, the high half first (the DWC3 write-64-hi-lo quirk).
  void XHCI_WRITE64(void *base, uint32_t offset, uint64_t value) override
  {
    Window w = {static_cast<volatile uint32_t *>(base)};
    pianoxhci::write64(w, offset, value);
    const uint64_t count = ++gWrites;
    if (logging(count))
      IOLog("PianoXhciHost: write64 #%llu +%#x = %#llx (high %#x first, then low %#x)\n", (unsigned long long)count, offset,
            (unsigned long long)value, uint32_t(value >> 32), uint32_t(value));
  }
};
OSDefineMetaClassAndStructors(PianoXhciHost, AppleUSBXHCI)

extern "C" {

// The kext's module start/stop (tooling/k27/kext/PianoXhciHost.json). Nothing is driven from here: the class starts through IOKit
// matching on PianoUsbHost's nub. The line is the boot-time evidence that the A2 subclass is in this collection and that its
// instance size is the original's (a size the static_assert of the generated header already proved at compile time).
kern_return_t piano_xhcihost_start(kmod_info_t *, void *)
{
  IOLog("PianoXhciHost: linked (AppleUSBXHCI subclass, 3 overrides: desired power, read64, write64; instance size %u bytes, the "
        "original's); it matches only PianoUsbHostNub (piano-usbhost=6 piano-usbhost-a2=1)\n", unsigned(sizeof(PianoXhciHost)));
  return KERN_SUCCESS;
}

kern_return_t piano_xhcihost_stop(kmod_info_t *, void *)
{
  IOLog("PianoXhciHost: stop (%llu read64, %llu write64)\n", (unsigned long long)gReads, (unsigned long long)gWrites);
  return KERN_SUCCESS;
}

}   // extern "C"
