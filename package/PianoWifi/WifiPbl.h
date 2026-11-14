// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 4, what comes before the boot loader's doorbell and what tells afterwards why it refused (docs/drivers/wifi.md
// §10.23, the fourth round of 2026-11-12). Portable over the owner's register callbacks; host tests: tests/bringup_host.cpp.
//
// The boot loader (the chip's PBL) took the image's address and refused 45 ms after the doorbell, `BHI status ffffffff, error code
// ef0e0003, debug 20000000 00000000 00080000` (the address and the size it was given), in every boot of the day, with physical and
// with 32-bit addresses, with the NoC vote and with the partial chip reset; no PCI, AER or SMMU fault anywhere. What the chip's
// own host does before that doorbell and this driver did not (Ubuntu's ath12k `pci.c` power-up and the MHI host's
// `mhi_prepare_for_power_up` / `mhi_async_power_up`, behaviour only, nothing copied), and the measurements that decide between
// "the chip never read the image" and "it read it and refused it":
//   chipPowerOnReset  the whole power-on recipe (WifiHw.h has the registers): the chip's PARF LTSSM word, its GCC hot-reset bit,
//                     the PCIe interrupts, the QFPROM's fuse-blow supply (VDD4BLOW) off, the BHIe vector words, the Q6 cookie, the
//                     warm-boot entry, the reset cause, the SoC global reset, MHICTRL's reset. Every value before and after is kept.
//   msiEnable         the endpoint's MSI on, one vector, its address a sink page in the DMA window: the MHI host runs the boot with
//                     MSI enabled, and the sink says afterwards whether the chip's writes reach the window at all.
//   rasDes*           the DesignWare TLP counters of either end of the link: how many reads the chip sent and the root port
//                     received, how many completions with and without data came back, and the error counts.
//   imageHash         the image as the chip would read it (through the window's write-combined mapping, i.e. from DRAM) against the
//                     stock file's pinned identity.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "WifiHw.h"
#include "WifiCe.h"
#include "WifiSha256.h"

namespace pianowifi {

// ------------------------------------------------------------------------------------------- the chip-side power-on reset
struct ChipReset {
  uint32_t ltssmBefore = 0, ltssm = 0, hotResetBefore = 0, hotReset = 0, qfpromBefore = 0, qfprom = 0;
  uint32_t cookie = 0, warm = 0, warmAfter = 0, cause = 0, globalBefore = 0, globalAfter = 0;
  uint32_t mhiStatusBefore = 0, mhiStatusAfter = 0, ee = 0;
  unsigned ltssmWrites = 0;
  bool linkLost = false;   // the global reset's last read was all ones: the link went down under it
};

// io reaches chip addresses (BAR0 directly below 0x80000, the rest through the window register); delayMs sleeps. bhiOff is the BHI
// block's offset (for the execution environment read at the end).
template <class Delay>
inline void chipPowerOnReset(const ChipIo &io, Delay delayMs, uint32_t bhiOff, ChipReset &r)
{
  auto rd = [&](uint32_t a) { return io.read(io.ctx, a); };
  auto wr = [&](uint32_t a, uint32_t v) { io.write(io.ctx, a, v); };
  renew(r);
  // The chip's PARF LTSSM word: retried while it does not read back, with a pause when the link does not answer.
  uint32_t v = r.ltssmBefore = rd(kChipParfLtssm);
  for (unsigned i = 0; v != kChipParfLtssmValue && i < kChipLtssmTries; ++i) {
    if (v == 0xffffffffu) delayMs(kChipLtssmSettleMs);
    wr(kChipParfLtssm, kChipParfLtssmValue);
    ++r.ltssmWrites;
    v = rd(kChipParfLtssm);
  }
  r.ltssm = v;
  r.hotResetBefore = rd(kChipGccHotReset);
  wr(kChipGccHotReset, r.hotResetBefore | kChipGccHotResetBit);
  r.hotReset = rd(kChipGccHotReset);
  delayMs(kChipLtssmSettleMs);
  wr(kChipPcieIntAllClear, 0xffffffffu);
  r.qfpromBefore = rd(kChipQfpromPwrCtrl);
  wr(kChipQfpromPwrCtrl, r.qfpromBefore & ~kChipQfpromVdd4Blow);
  r.qfprom = rd(kChipQfpromPwrCtrl);
  wr(kChipTxVecDb, 0);
  wr(kChipTxVecStatus, 0);
  wr(kChipRxVecDb, 0);
  wr(kChipRxVecStatus, 0);
  r.cookie = rd(kChipQ6Cookie);
  r.warm = rd(kChipWlaonWarmSwEntry);
  delayMs(kChipSocResetMs);
  wr(kChipWlaonWarmSwEntry, 0);   // keeps the Q6 off the warm-boot path
  delayMs(kChipSocResetMs);
  r.warmAfter = rd(kChipWlaonWarmSwEntry);
  r.cause = rd(kChipWlaonResetCause);   // clears on read
  r.globalBefore = rd(kChipSocGlobalReset);
  wr(kChipSocGlobalReset, r.globalBefore | kChipSocGlobalResetBit);
  (void)rd(kChipSocGlobalReset);
  delayMs(kChipSocResetMs);
  wr(kChipSocGlobalReset, r.globalBefore & ~kChipSocGlobalResetBit);
  (void)rd(kChipSocGlobalReset);
  delayMs(kChipSocResetMs);
  r.globalAfter = rd(kChipSocGlobalReset);
  r.linkLost = r.globalAfter == 0xffffffffu;
  r.mhiStatusBefore = rd(kMhiStatus);
  wr(kMhiCtrl, kMhiCtrlReset);   // clears a SYSERR the global reset may leave
  delayMs(kChipSocResetMs);
  r.mhiStatusAfter = rd(kMhiStatus);
  r.ee = bhiOff ? rd(bhiOff + kBhiExecEnv) : 0;
}

// ------------------------------------------------------------------------------------------------- the endpoint's MSI
struct MsiSetup {
  uint32_t cap = 0, ctlBefore = 0, ctl = 0, addressLo = 0, addressHi = 0, data = 0, command = 0;
  bool is64 = false, enabled = false;
  const char *why = nullptr;   // why it was not enabled
};
// rd/wr reach the endpoint's configuration space (dword offsets). cap is the MSI capability level 3 found (0: none).
template <class Read, class Write>
inline bool msiEnable(MsiSetup &m, Read rd, Write wr, uint32_t cap, uint64_t address, uint16_t data)
{
  renew(m);
  m.cap = cap;
  if (!cap || (cap & 3) || cap < 0x40 || cap > 0xf0) return m.why = "no MSI capability", false;
  const uint32_t head = rd(cap);
  if ((head & 0xffu) != 0x05) return m.why = "the capability is not MSI", false;
  m.ctlBefore = head >> 16;
  m.is64 = (m.ctlBefore & kMsiCtl64) != 0;
  if ((address & 3) || (!m.is64 && (address >> 32))) return m.why = "the sink's address does not fit the capability", false;
  wr(cap, (head & 0xffffu) | (m.ctlBefore & ~(kMsiCtlEnable | kMsiCtlMmeMask)) << 16);   // off while it is programmed
  wr(cap + 4, uint32_t(address));
  if (m.is64) wr(cap + 8, uint32_t(address >> 32));
  wr(cap + (m.is64 ? 0xc : 8), data);
  if (m.ctlBefore & kMsiCtlMask) wr(cap + (m.is64 ? 0x10 : 0xc), 0);   // no vector masked
  wr(0x04, (rd(0x04) & 0xffffu) | kCommandIntxDisable);                 // INTx off; the status half is write-one-to-clear: 0
  wr(cap, (head & 0xffffu) | ((m.ctlBefore & ~kMsiCtlMmeMask) | kMsiCtlEnable) << 16);   // one vector, on
  m.ctl = rd(cap) >> 16;
  m.addressLo = rd(cap + 4);
  m.addressHi = m.is64 ? rd(cap + 8) : 0;
  m.data = rd(cap + (m.is64 ? 0xc : 8)) & 0xffffu;
  m.command = rd(0x04);
  m.enabled = (m.ctl & kMsiCtlEnable) && m.addressLo == uint32_t(address) && m.addressHi == uint32_t(address >> 32) &&
              m.data == data;
  if (!m.enabled) m.why = "the capability did not read back";
  return m.enabled;
}

// ------------------------------------------------------------------------------------- the TLP counters (RAS DES)
struct RasDes {
  uint32_t at = 0;              // the capability's offset (0: not found)
  uint32_t on = 0;              // the events whose counter reads as enabled, a bit each (kRasEvents order)
  uint32_t start[kRasEventCount] = {}, delta[kRasEventCount] = {};
};
// The vendor-specific capability with ID 2 revision 4 in the extended list, on a controller whose vendor is Qualcomm.
template <class Read>
inline uint32_t rasDesFind(Read rd)
{
  if ((rd(0) & 0xffffu) != kEndpointVendor) return 0;   // Qualcomm's vendor ID, the root port's and the chip's alike
  uint32_t at = kExtCapStart;
  for (unsigned guard = 0; at >= kExtCapStart && at + 0x10 <= 0x1000 && !(at & 3) && guard < 48; ++guard) {
    const uint32_t h = rd(at);
    if (!h || h == 0xffffffffu) return 0;
    if ((h & 0xffffu) == kExtCapVendor) {
      const uint32_t v = rd(at + 4);
      if ((v & 0xffffu) == kRasDesVsecId && ((v >> 16) & 0xfu) == kRasDesVsecRev) return at;
    }
    at = (h >> 20) & 0xffcu;
  }
  return 0;
}
inline uint32_t rasDesSelect(const RasEvent &e) { return uint32_t(e.group) << 24 | uint32_t(e.event) << 16; }
// Each event's counter switched on and its starting value kept (the counters are never cleared: a delta is what level 4 logs).
template <class Read, class Write>
inline void rasDesArm(RasDes &r, Read rd, Write wr)
{
  const uint32_t at = r.at;
  renew(r);
  r.at = at;
  if (!at) return;
  for (unsigned i = 0; i < kRasEventCount; ++i) {
    wr(at + kRasDesCtrl, rasDesSelect(kRasEvents[i]) | kRasDesEnableOn);
    if (rd(at + kRasDesCtrl) & kRasDesStatus) r.on |= 1u << i;
    r.start[i] = rd(at + kRasDesData);
  }
}
template <class Read, class Write>
inline void rasDesRead(RasDes &r, Read rd, Write wr)
{
  if (!r.at) return;
  for (unsigned i = 0; i < kRasEventCount; ++i) {
    wr(r.at + kRasDesCtrl, rasDesSelect(kRasEvents[i]));   // the enable field 0: select only
    r.delta[i] = rd(r.at + kRasDesData) - r.start[i];
  }
}
inline bool rasOn(const RasDes &r, uint8_t group, uint8_t event)
{
  for (unsigned i = 0; i < kRasEventCount; ++i)
    if (kRasEvents[i].group == group && kRasEvents[i].event == event) return r.at && ((r.on >> i) & 1u);
  return false;
}
inline uint32_t rasDelta(const RasDes &r, uint8_t group, uint8_t event)
{
  for (unsigned i = 0; i < kRasEventCount; ++i)
    if (kRasEvents[i].group == group && kRasEvents[i].event == event) return (r.on >> i) & 1u ? r.delta[i] : 0;
  return 0;
}

// ------------------------------------------------------------------- what the boot loader says it read from its fuses
// The MSM hardware ID and the OEM public key hash as BHI reports them, summarised without their values: the non-zero words and the
// first four bytes of a SHA-256 over all of them (the same summary in two boots, or before and after the doorbell, means the same
// words).
struct BhiIdentity {
  unsigned hwIdNonZero = 0, pkHashNonZero = 0, pkHashAllOnes = 0;
  uint32_t digest = 0;
};
template <class Read>
inline BhiIdentity bhiIdentity(Read rd, uint32_t bhiOff)
{
  BhiIdentity id;
  Sha256 s;
  for (uint32_t i = 0; i < kBhiMsmHwIdWords + kBhiOemPkHashWords; ++i) {
    const bool hw = i < kBhiMsmHwIdWords;
    const uint32_t w = rd(bhiOff + (hw ? kBhiMsmHwId + 4 * i : kBhiOemPkHash + 4 * (i - kBhiMsmHwIdWords)));
    if (w && hw) ++id.hwIdNonZero;
    if (w && !hw) ++id.pkHashNonZero;
    if (w == 0xffffffffu && !hw) ++id.pkHashAllOnes;
    const uint8_t b[4] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24)};
    sha256Update(s, b, 4);
  }
  uint8_t d[32];
  sha256Final(s, d);
  id.digest = uint32_t(d[0]) << 24 | uint32_t(d[1]) << 16 | uint32_t(d[2]) << 8 | d[3];
  return id;
}

// ------------------------------------------------------------------------------- the image as the chip would read it
// The parts in file order (the boot loader's buffer, then each segment), each read through a bounce buffer the owner gives (the
// window is write-combined: a byte loop over it would be one bus read per byte).
struct Span { const uint8_t *at; uint32_t bytes; };
template <class Copy>
inline void hashSpans(const Span *spans, unsigned count, uint8_t *bounce, uint32_t bounceBytes, Copy copy, uint8_t digest[32])
{
  Sha256 s;
  for (unsigned i = 0; i < count; ++i)
    for (uint32_t done = 0; done < spans[i].bytes;) {
      const uint32_t n = spans[i].bytes - done < bounceBytes ? spans[i].bytes - done : bounceBytes;
      copy(bounce, spans[i].at + done, n);
      sha256Update(s, bounce, n);
      done += n;
    }
  sha256Final(s, digest);
}

}  // namespace pianowifi
