// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi levels 2 and 3 as one portable sequence: what is written, in which order, with which read-backs, waits and timeouts.
// Every hardware access goes through the Hw callbacks (a window index and an offset inside it), so the host tests
// (tests/wifi_host.cpp) run the whole ladder against register models and can make any step fail. Nothing here maps memory, sleeps
// by itself or touches an address outside the windows the owner mapped.
//
// Level 2 (power): the six PMU rails by RPMh (mode, voltage, enable, in piano's DT supply order), read back over the SPMI observer
// before and after; PERST# driven low; CLKREQ# given to its PCIe function; WLAN_EN low 150 ms, then high, then the PMU's 50 ms.
// Level 3 (link): the two GDSCs out of collapse, the aux/rchng RCG sources, the branch votes (under the shared GCC vote lock), the
// PHY's TCSR reference gate, the two block resets, the PARF set-up, the PHY sequence (WifiPhySeq.h), the BDF-to-SID entry of the
// endpoint, the root port's bus numbers and target speed, PERST# released at least 100 ms after it went low, the LTSSM, the wait for
// L0, the configuration window and the endpoint's `17cb:110e`, then the memory window, BAR0's size and its address.
//
// The rules every step keeps (docs/drivers/wifi.md section 8):
//   - a vote only ever adds: a rail gets its mode and its enable, and a voltage only when that is not below what the PMIC reports;
//     `vreg_l3g_1p2` and `vreg_l1f_0p88` (the UFS PHY's PLL and refgen) are read, never voted, never lowered;
//   - every register write goes through Hw::write with what it is and is read back where a read-back means anything;
//   - every poll is bounded and its timeout is an error, never a retry loop; a failed step stops the ladder there and leaves the
//     hardware as safe as it can: WLAN_EN low and PERST# asserted (`safeOff`);
//   - nothing is written to a block whose power domain or interface clocks are off (level 1's gates, WifiSurvey.h).
#pragma once
#include <stdint.h>
#include "WifiHw.h"
#include "WifiPhySeq.h"
#include "WifiSurvey.h"

namespace pianowifi {

// The mapped windows. kWinCfg is the endpoint's 1 MiB configuration window, which is read only after iATU region 0 is enabled.
enum Win : unsigned { kWinGcc, kWinTlmm, kWinTcsr, kWinParf, kWinPhy, kWinDbi, kWinAtu, kWinCfg, kWinCount };

enum Step : int {
  kStepRailsRead = 201, kStepRailVote = 202, kStepRailsCheck = 203, kStepPerst = 204, kStepWlanEn = 205,
  kStepGdsc = 301, kStepClocks = 302, kStepResets = 303, kStepParf = 304, kStepPhy = 305, kStepSid = 306, kStepRootPort = 307,
  kStepLtssm = 308, kStepLink = 309, kStepEndpoint = 310, kStepBar = 311,
};

enum Error : int {
  kErrNone = 0, kErrStopped = -1, kErrArgument = -2, kErrNoRpmh = -3, kErrRailRead = -4, kErrRailVote = -5, kErrRailDropped = -6,
  kErrPin = -7, kErrGdsc = -8, kErrRcg = -9, kErrBranch = -10, kErrClkRef = -11, kErrReset = -12, kErrParf = -13, kErrPhy = -14,
  kErrSid = -15, kErrLink = -16, kErrAtu = -17, kErrEndpoint = -18, kErrBar = -19,
};

inline const char *errorName(int error)
{
  switch (error) {
  case kErrNone: return "none";
  case kErrStopped: return "stopped here on request";
  case kErrArgument: return "bad arguments";
  case kErrNoRpmh: return "no RPMh path";
  case kErrRailRead: return "a rail's PMIC state could not be read";
  case kErrRailVote: return "a rail vote was refused";
  case kErrRailDropped: return "a rail came back lower or disabled";
  case kErrPin: return "a pin did not read back";
  case kErrGdsc: return "a power domain did not come up";
  case kErrRcg: return "an RCG update did not finish";
  case kErrBranch: return "a branch clock did not start";
  case kErrClkRef: return "the PHY reference clock gate did not set";
  case kErrReset: return "a block reset stayed asserted";
  case kErrParf: return "a PARF register did not read back";
  case kErrPhy: return "the PHY did not report ready";
  case kErrSid: return "the BDF-to-SID entry did not read back";
  case kErrLink: return "no L0 within the timeout";
  case kErrAtu: return "an iATU region did not enable";
  case kErrEndpoint: return "the endpoint did not answer 17cb:110e";
  case kErrBar: return "BAR0 could not be sized or placed";
  default: return "?";
  }
}

struct Options {
  unsigned level = 0;                  // piano-wifi
  unsigned gen = kDefaultGen;          // piano-wifi-gen: the target link speed (1..3; Gen3 needs cx/mx at NOM, which we do not vote)
  unsigned physeq = 1;                 // piano-wifi-physeq=0: only the three frame writes of the PHY sequence
  unsigned rcg = 1;                    // piano-wifi-rcg=0: leave the aux/rchng RCG sources as the boot stages left them
  unsigned blind = 0;                  // piano-wifi-blind=1: vote a rail whose PMIC state could not be read (off: refuse)
  unsigned release = 0;                // piano-wifi-release=1: on a failure, withdraw the enable of the rails this driver enabled
  unsigned wlanEnLowMs = kWlanEnLowMs;
  unsigned pmuMs = kPmuPowerUpMs;
  unsigned linkMs = kLinkTrainMs;
  int stopAfter = 0;                   // piano-wifi-stop=N: return after step N (debugging)
};

struct VrmCmd { uint32_t reg, value; };

struct Hw {
  void *ctx = nullptr;
  uint32_t (*read)(void *ctx, unsigned win, uint32_t off) = nullptr;
  // Writes and completes with a barrier. `what` names the write for the log (the owner rate-limits by that name).
  void (*write)(void *ctx, unsigned win, uint32_t off, uint32_t value, const char *what) = nullptr;
  void (*udelay)(void *ctx, unsigned us) = nullptr;
  void (*msleep)(void *ctx, unsigned ms) = nullptr;
  uint64_t (*now_ms)(void *ctx) = nullptr;
  // PianoA830's sm8750_gcc_vote_owner around a read-modify-write of 0x52000 / 0x52008 / the collapse word. 0: taken (released).
  int (*vote_lock)(void *ctx, int take) = nullptr;
  // Up to three commands of one VRM resource in one transaction of an ACTIVE TCS of apps DRV-2, under the DRV-2 lock. 0, or negative.
  int (*vrm)(void *ctx, const char *resource, const VrmCmd *cmds, unsigned count, uint32_t *address) = nullptr;
  // One regulator read over the SPMI observer channel (never a PMIC write). 0, or negative.
  int (*pmic)(void *ctx, uint16_t ppid, uint8_t *status, uint8_t *enable, uint8_t *mode, uint16_t *millivolts) = nullptr;
  void (*step)(void *ctx, int step, const char *what) = nullptr;   // one line as each step starts
};

struct RailVote {
  uint16_t ppid = 0;                   // filled by the owner from level 1's survey (0: the regulator was not found)
  uint32_t address = 0;                // the RPMh VRM address from cmd-db (the owner fills it; 0: not found)
  int rc = 0;
  bool read = false, readAfter = false;
  uint8_t status = 0, enable = 0, mode = 0, statusAfter = 0, enableAfter = 0, modeAfter = 0;
  uint16_t millivolts = 0, millivoltsAfter = 0;
  uint16_t voted = 0;                  // the set point sent (0: none sent)
  bool sentEnable = false, keptVoltage = false, enabledByUs = false;
};

struct PowerState {
  RailVote rails[kRailCount];
  uint32_t wlanEnCfg = 0, wlanEnIo = 0, perstCfg = 0, perstIo = 0, clkreqCfg = 0, clkreqIo = 0, swCtrlIo = 0;
  uint64_t perstAssertedMs = 0;
  unsigned voted = 0, kept = 0;
  int step = 0, error = 0;
};

struct LinkState {
  uint32_t collapseBefore = 0, collapseAfter = 0, gdsc = 0, gdscCfg = 0, phyGdsc = 0, phyGdscCfg = 0;
  unsigned gdscPolls = 0;
  uint32_t vote8 = 0, vote0 = 0, auxRcg = 0, auxCfg = 0, rchngRcg = 0, rchngCfg = 0, pipeMux = 0, clkref = 0;
  uint32_t branchOff = 0, pipeCbcr = 0;
  int voteLock = 0;                    // what vote_lock returned (0: taken)
  uint32_t bcr[2] = {0, 0};
  uint32_t parfDeviceType = 0, parfPhyCtrl = 0, parfDbiBase = 0, parfAtuBase = 0, parfSlvSpaceHi = 0, parfSysCtrl = 0, parfLtssm = 0;
  unsigned phyWrites = 0, phyPolls = 0;
  uint32_t pcsStatus = 0;
  uint8_t sidHash = 0;
  uint32_t sidEntry = 0, sidCfg = 0;
  uint32_t rootId = 0, busNumbers = 0, rootCommand = 0, expCap = 0, linkCtl2 = 0;
  uint32_t perstLowMs = 0, linkMs = 0, debug0 = 0, debug1 = 0, linkStatus = 0;
  uint32_t atuCtrl2[2] = {0, 0};
  uint32_t endpointId = 0, endpointClass = 0, endpointSubsys = 0, endpointCap = 0, endpointMsiCap = 0;
  unsigned endpointPolls = 0;
  uint32_t barMask = 0, barValue = 0, command = 0, memBaseLimit = 0;
  uint64_t barAddress = 0;
  uint32_t barBytes = 0;
  int step = 0, error = 0;
};

// ------------------------------------------------------------------------------------------------------------ small helpers
// The TLMM control word of a pin state: pull (-1 down, 0 none, 1 up), function, drive in mA (2..16), output enable.
inline uint32_t pinConfig(int pull, unsigned function, unsigned driveMa, bool output)
{
  const uint32_t pullBits = pull < 0 ? 1u : pull > 0 ? 3u : 0u;
  const uint32_t drive = driveMa >= 2 && driveMa <= 16 ? driveMa / 2 - 1 : 0u;
  return pullBits | (function & 0xfu) << 2 | (drive & 7u) << 6 | (output ? 1u << 9 : 0u);
}

// The BDF-to-SID hash of a requester ID: CRC-8, polynomial 0x07, MSB first, the high byte then the low one (PCI; PAGANI).
inline uint8_t bdfHash(uint16_t bdf)
{
  uint8_t crc = 0;
  const uint8_t bytes[2] = {uint8_t(bdf >> 8), uint8_t(bdf)};
  for (unsigned i = 0; i < 2; ++i) {
    crc = uint8_t(crc ^ bytes[i]);
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc & 0x80u) ? uint8_t(uint8_t(crc << 1) ^ kBdfHashPoly) : uint8_t(crc << 1);
  }
  return crc;
}

// A memory BAR's size from the mask the all-ones probe read back: 0 when it is not a memory BAR, or the mask says nothing.
inline uint64_t barSize(uint32_t mask)
{
  if (mask == 0xffffffffu || (mask & 1u)) return 0;            // unimplemented, or an I/O BAR
  const uint32_t bits = mask & ~0xfu;
  if (!bits) return 0;
  const uint64_t size = uint64_t(~bits) + 1;
  return (size & (size - 1)) ? 0 : size;                        // a size that is not a power of two is not a BAR mask
}
inline bool barIs64(uint32_t mask) { return (mask & 6u) == 4u; }
// Where a BAR of `size` bytes goes in the 32-bit memory window [base, base + bytes): the first address aligned to its own size.
// 0 when it does not fit.
inline uint64_t barPlace(uint64_t base, uint64_t bytes, uint64_t size)
{
  if (!size || size > bytes) return 0;
  const uint64_t at = (base + size - 1) & ~(size - 1);
  return at + size <= base + bytes ? at : 0;
}
// The type 1 header's memory base/limit word for a window (bits 15:4 base 31:20, bits 31:20 limit 31:20).
inline uint32_t memBaseLimit(uint64_t base, uint64_t bytes)
{
  const uint64_t last = base + bytes - 1;
  return uint32_t((last & 0xfff00000u) | ((base >> 16) & 0xfff0u));
}
// A configuration dword 0 that means "the endpoint has not answered yet": all ones, or the retry status's vendor 0x0001.
inline bool idPending(uint32_t id) { return id == 0xffffffffu || (id & 0xffffu) == 0x0001u; }
inline bool idIsEndpoint(uint32_t id) { return id == (uint32_t(kEndpointDevice) << 16 | kEndpointVendor); }

// ------------------------------------------------------------------------------------------------------------ the sequence
namespace detail {

inline uint32_t rd(const Hw &hw, unsigned win, uint32_t off) { return hw.read(hw.ctx, win, off); }
inline void wr(const Hw &hw, unsigned win, uint32_t off, uint32_t value, const char *what)
{
  hw.write(hw.ctx, win, off, value, what);
}
inline void setBits(const Hw &hw, unsigned win, uint32_t off, uint32_t bits, const char *what)
{
  wr(hw, win, off, rd(hw, win, off) | bits, what);
}
inline void clearBits(const Hw &hw, unsigned win, uint32_t off, uint32_t bits, const char *what)
{
  wr(hw, win, off, rd(hw, win, off) & ~bits, what);
}
inline void announce(const Hw &hw, int step, const char *what)
{
  if (hw.step) hw.step(hw.ctx, step, what);
}
// Output level first, then the configuration, then the level again (SHENG's setPin): the pad never drives the old level once its
// output is enabled.
inline void setPin(const Hw &hw, unsigned gpio, uint32_t config, int level, const char *what)
{
  const uint32_t at = gpio * kTlmmStride;
  if (level >= 0) wr(hw, kWinTlmm, at + 4, level ? 2u : 0u, what);
  wr(hw, kWinTlmm, at, config, what);
  if (level >= 0) wr(hw, kWinTlmm, at + 4, level ? 2u : 0u, what);
}
inline bool waitClear(const Hw &hw, unsigned win, uint32_t off, uint32_t mask, unsigned polls, unsigned stepUs, unsigned *used)
{
  unsigned n = 0;
  for (; n < polls && (rd(hw, win, off) & mask); ++n) hw.udelay(hw.ctx, stepUs);
  if (used) *used = n;
  return !(rd(hw, win, off) & mask);
}
inline bool waitSet(const Hw &hw, unsigned win, uint32_t off, uint32_t mask, unsigned polls, unsigned stepUs, unsigned *used)
{
  unsigned n = 0;
  for (; n < polls && !(rd(hw, win, off) & mask); ++n) hw.udelay(hw.ctx, stepUs);
  if (used) *used = n;
  return (rd(hw, win, off) & mask) != 0;
}
inline bool hwUsable(const Hw &hw)
{
  return hw.read && hw.write && hw.udelay && hw.msleep && hw.now_ms;
}

}  // namespace detail

// WLAN_EN low and PERST# asserted: where a failure of any step above level 1 leaves the hardware. Rails keep their votes (a
// disable could cut a rail Bluetooth or another consumer needs); piano-wifi-release=1 withdraws the enable of the rails this
// driver enabled itself, and nothing else.
inline void safeOff(const Hw &hw, PowerState &p, const Options &opt)
{
  using namespace detail;
  if (!detail::hwUsable(hw)) return;
  setPin(hw, kPins[kPinWlanEn].gpio, pinConfig(-1, 0, 2, true), 0, "WLAN_EN low (safe state)");
  setPin(hw, kPins[kPinPerst].gpio, pinConfig(-1, 0, 2, true), 0, "PERST# asserted (safe state)");
  if (!opt.release || !hw.vrm) return;
  for (unsigned i = 0; i < kVotedRails; ++i) {
    RailVote &r = p.rails[i];
    if (!r.enabledByUs || !r.address) continue;
    const VrmCmd off = {kVrmEnable, 0};
    r.rc = hw.vrm(hw.ctx, kRails[i].cmdDb, &off, 1, &r.address);
    r.enabledByUs = false;
  }
}

// ---- level 2 ----------------------------------------------------------------------------------------------------------------
// The owner fills p.rails[i].ppid and .address (from level 1's PMIC survey and cmd-db lookup) before calling this.
inline int powerUp(PowerState &p, const Hw &hw, const Options &opt)
{
  using namespace detail;
  if (!hwUsable(hw)) return p.error = kErrArgument, p.step = -kStepRailsRead, kErrArgument;
  if (!hw.vrm) return p.error = kErrNoRpmh, p.step = -kStepRailVote, kErrNoRpmh;

  auto readRail = [&](unsigned i, bool after) {
    RailVote &r = p.rails[i];
    uint8_t status = 0, enable = 0, mode = 0;
    uint16_t mv = 0;
    const bool ok = hw.pmic && r.ppid && !hw.pmic(hw.ctx, r.ppid, &status, &enable, &mode, &mv);
    if (after) {
      r.readAfter = ok;
      r.statusAfter = status;
      r.enableAfter = enable;
      r.modeAfter = mode;
      r.millivoltsAfter = mv;
    } else {
      r.read = ok;
      r.status = status;
      r.enable = enable;
      r.mode = mode;
      r.millivolts = mv;
    }
  };
  auto failed = [&](int step, int error) {
    p.step = -step;
    p.error = error;
    safeOff(hw, p, opt);
    return error;
  };

  // Every rail (and the two PHY LDOs, read only) as the PMIC has them now. A rail whose state cannot be read is not voted unless
  // piano-wifi-blind=1: a voltage command sent blind could be below what another consumer holds.
  announce(hw, kStepRailsRead, "the rails as the PMIC has them (SPMI observer reads)");
  for (unsigned i = 0; i < kRailCount; ++i) readRail(i, false);
  for (unsigned i = 0; i < kVotedRails; ++i) {
    if (!p.rails[i].address) return failed(kStepRailsRead, kErrRailVote);          // no cmd-db resource: nothing to vote
    if (!p.rails[i].read && !opt.blind) return failed(kStepRailsRead, kErrRailRead);
  }
  p.step = kStepRailsRead;
  if (opt.stopAfter == kStepRailsRead) return p.error = kErrStopped, kErrStopped;

  // The six PMU rails in piano's DT supply order, each with mode (NPM), voltage and enable in one TCS transaction. The voltage
  // asked for is what the chip needs (piano's own DT minimum) unless the PMIC already reports more, in which case that value is
  // sent again (never less) or, above the DT maximum, no voltage command is sent at all. The enable only ever adds a vote.
  announce(hw, kStepRailVote, "the six PMU rails by RPMh: mode NPM, voltage, enable (DRV-2 lock per transaction)");
  for (unsigned order = 1; order <= kVotedRails; ++order) {
    unsigned i = 0;
    while (i < kVotedRails && kRails[i].order != order) ++i;
    if (i == kVotedRails) continue;
    const Rail &rail = kRails[i];
    RailVote &r = p.rails[i];
    uint32_t mv = rail.minMv;
    bool sendVoltage = true;
    if (r.read && r.millivolts > mv) {
      if (r.millivolts <= rail.maxMv) mv = r.millivolts;    // keep what it has
      else sendVoltage = false;                             // above the range: not ours to lower, and not a value to send
    }
    VrmCmd cmds[3];
    unsigned n = 0;
    cmds[n++] = {kVrmMode, kVrmModeNpm};
    if (sendVoltage) cmds[n++] = {kVrmVoltage, mv};
    cmds[n++] = {kVrmEnable, 1};
    r.rc = hw.vrm(hw.ctx, rail.cmdDb, cmds, n, &r.address);
    r.voted = sendVoltage ? uint16_t(mv) : 0;
    r.keptVoltage = !sendVoltage;
    r.sentEnable = r.rc == 0;
    r.enabledByUs = r.rc == 0 && r.read && !(r.enable & 0x80u);
    if (sendVoltage) ++p.voted;
    else ++p.kept;
    if (r.rc) return failed(kStepRailVote, kErrRailVote);
  }
  hw.msleep(hw.ctx, 5);
  p.step = kStepRailVote;
  if (opt.stopAfter == kStepRailVote) return p.error = kErrStopped, kErrStopped;

  // Read every rail back, the two PHY LDOs with them: no rail may have come back lower or disabled, and `vreg_l3g_1p2` (the UFS
  // PHY's PLL) and `vreg_l1f_0p88` must be exactly as they were.
  announce(hw, kStepRailsCheck, "the rails after the votes; the UFS PHY's supplies unchanged");
  for (unsigned i = 0; i < kRailCount; ++i) readRail(i, true);
  for (unsigned i = 0; i < kRailCount; ++i) {
    const RailVote &r = p.rails[i];
    if (!r.read || !r.readAfter) continue;
    if (r.millivoltsAfter < r.millivolts) return failed(kStepRailsCheck, kErrRailDropped);
    if ((r.enable & 0x80u) && !(r.enableAfter & 0x80u)) return failed(kStepRailsCheck, kErrRailDropped);
  }
  p.step = kStepRailsCheck;
  if (opt.stopAfter == kStepRailsCheck) return p.error = kErrStopped, kErrStopped;

  // PERST# low before anything can train, CLKREQ# to the controller's function (an input with its pull-up).
  announce(hw, kStepPerst, "PERST# low, CLKREQ# to its PCIe function");
  setPin(hw, kPins[kPinPerst].gpio, pinConfig(-1, 0, 2, true), 0, "PERST# low");
  p.perstAssertedMs = hw.now_ms(hw.ctx);
  setPin(hw, kPins[kPinClkReq].gpio, pinConfig(1, 1, 2, false), -1, "CLKREQ# function");
  p.perstCfg = rd(hw, kWinTlmm, kPins[kPinPerst].gpio * kTlmmStride);
  p.perstIo = rd(hw, kWinTlmm, kPins[kPinPerst].gpio * kTlmmStride + 4);
  p.clkreqCfg = rd(hw, kWinTlmm, kPins[kPinClkReq].gpio * kTlmmStride);
  p.clkreqIo = rd(hw, kWinTlmm, kPins[kPinClkReq].gpio * kTlmmStride + 4);
  if (decodePin(p.perstCfg, p.perstIo).out) return failed(kStepPerst, kErrPin);
  p.step = kStepPerst;
  if (opt.stopAfter == kStepPerst) return p.error = kErrStopped, kErrStopped;

  // WLAN_EN: low long enough for an earlier start to have stopped, then high, then the PMU's power-up time. SW_CTRL is the chip's
  // answer; it is reported, not required (no boot has shown yet what it reads on piano).
  announce(hw, kStepWlanEn, "WLAN_EN low, then high; the PMU's power-up time; SW_CTRL read");
  setPin(hw, kPins[kPinWlanEn].gpio, pinConfig(-1, 0, 2, true), 0, "WLAN_EN low");
  hw.msleep(hw.ctx, opt.wlanEnLowMs);
  setPin(hw, kPins[kPinWlanEn].gpio, pinConfig(-1, 0, 2, true), 1, "WLAN_EN high");
  hw.msleep(hw.ctx, opt.pmuMs + 10);
  p.wlanEnCfg = rd(hw, kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride);
  p.wlanEnIo = rd(hw, kWinTlmm, kPins[kPinWlanEn].gpio * kTlmmStride + 4);
  p.swCtrlIo = rd(hw, kWinTlmm, kPins[kPinSwCtrl].gpio * kTlmmStride + 4);
  if (!decodePin(p.wlanEnCfg, p.wlanEnIo).out) return failed(kStepWlanEn, kErrPin);
  p.step = kStepWlanEn;
  if (opt.stopAfter == kStepWlanEn) return p.error = kErrStopped, kErrStopped;
  return kErrNone;
}

// ---- level 3 ----------------------------------------------------------------------------------------------------------------
inline int linkUp(LinkState &l, PowerState &p, const Hw &hw, const Options &opt)
{
  using namespace detail;
  if (!hwUsable(hw)) return l.error = kErrArgument, l.step = -kStepGdsc, kErrArgument;
  auto failed = [&](int step, int error) {
    l.step = -step;
    l.error = error;
    safeOff(hw, p, opt);
    return error;
  };
  auto stop = [&](int step) {
    l.step = step;
    if (opt.stopAfter != step) return false;
    l.error = kErrStopped;
    return true;
  };

  // The two power domains: a set bit in the collapse word keeps one collapsed (PCIe0 bit 0, its PHY bit 2). The word is shared, so
  // the read-modify-write happens under PianoA830's GCC vote lock.
  announce(hw, kStepGdsc, "the PCIe0 and PCIe0 PHY power domains out of collapse");
  l.collapseBefore = rd(hw, kWinGcc, kGccCollapse);
  if (l.collapseBefore & (kCollapsePcie0 | kCollapsePcie0Phy)) {
    l.voteLock = hw.vote_lock ? hw.vote_lock(hw.ctx, 1) : 0;
    if (l.voteLock) return failed(kStepGdsc, kErrGdsc);
    clearBits(hw, kWinGcc, kGccCollapse, kCollapsePcie0 | kCollapsePcie0Phy, "GCC collapse vote: release PCIe0 and its PHY");
    if (hw.vote_lock) hw.vote_lock(hw.ctx, 0);
  }
  l.collapseAfter = rd(hw, kWinGcc, kGccCollapse);
  unsigned polls = 0;
  const bool up = waitSet(hw, kWinGcc, kGccPcie0Gdsc, kGdscPwrOn, 100, 10, &polls) &&
                  waitSet(hw, kWinGcc, kGccPcie0PhyGdsc, kGdscPwrOn, 100, 10, nullptr);
  l.gdscPolls = polls;
  l.gdsc = rd(hw, kWinGcc, kGccPcie0Gdsc);
  l.gdscCfg = rd(hw, kWinGcc, kGccPcie0Gdsc + 4);
  l.phyGdsc = rd(hw, kWinGcc, kGccPcie0PhyGdsc);
  l.phyGdscCfg = rd(hw, kWinGcc, kGccPcie0PhyGdsc + 4);
  if (!up || !gdscOn(l.gdsc) || !gdscOn(l.phyGdsc)) return failed(kStepGdsc, kErrGdsc);
  if (stop(kStepGdsc)) return kErrStopped;

  // Sources first (an RCG takes a new CFG when CMD bit 0 is set), the pipe mux parked on the TCXO until the PHY drives its pipe
  // clock, then the branch votes (bits only ever set, under the lock word), then the PHY's reference clock gate in TCSR.
  announce(hw, kStepClocks, "the aux/rchng sources, the pipe mux on TCXO, the branch votes, the TCSR reference gate");
  if (opt.rcg) {
    wr(hw, kWinGcc, kGccPcie0AuxRcg + 4, kAuxCfgTcxo, "GCC pcie0 aux RCG: TCXO");
    setBits(hw, kWinGcc, kGccPcie0AuxRcg, kRcgUpdate, "GCC pcie0 aux RCG update");
    wr(hw, kWinGcc, kGccPcie0RchngRcg + 4, kRchngCfg100MHz, "GCC pcie0 rchng RCG: 100 MHz");
    setBits(hw, kWinGcc, kGccPcie0RchngRcg, kRcgUpdate, "GCC pcie0 rchng RCG update");
    // The roots are off here (nothing votes them yet), so an update may stay pending; it is judged again after the votes.
    (void)waitClear(hw, kWinGcc, kGccPcie0AuxRcg, kRcgUpdate, 100, 10, nullptr);
    (void)waitClear(hw, kWinGcc, kGccPcie0RchngRcg, kRcgUpdate, 100, 10, nullptr);
  }
  wr(hw, kWinGcc, kGccPcie0PipeMux, (rd(hw, kWinGcc, kGccPcie0PipeMux) & ~3u) | kPipeMuxTcxo, "GCC pcie0 pipe mux: TCXO");
  l.voteLock = hw.vote_lock ? hw.vote_lock(hw.ctx, 1) : 0;
  if (l.voteLock) return failed(kStepClocks, kErrBranch);
  if ((rd(hw, kWinGcc, kGccVote8) & kVote8Bits) != kVote8Bits)
    setBits(hw, kWinGcc, kGccVote8, kVote8Bits, "GCC branch votes 0x52008 (PCIe0)");
  if ((rd(hw, kWinGcc, kGccVote0) & kVote0Bits) != kVote0Bits)
    setBits(hw, kWinGcc, kGccVote0, kVote0Bits, "GCC branch votes 0x52000 (PCIe0 NoC, rchng)");
  if (hw.vote_lock) hw.vote_lock(hw.ctx, 0);
  setBits(hw, kWinTcsr, uint32_t(kTcsrPcie0ClkRef & 0xfff), kTcsrClkRefEnable, "TCSR pcie0 clkref enable");
  hw.udelay(hw.ctx, 200);
  l.vote8 = rd(hw, kWinGcc, kGccVote8);
  l.vote0 = rd(hw, kWinGcc, kGccVote0);
  l.clkref = rd(hw, kWinTcsr, uint32_t(kTcsrPcie0ClkRef & 0xfff));
  l.auxRcg = rd(hw, kWinGcc, kGccPcie0AuxRcg);
  l.auxCfg = rd(hw, kWinGcc, kGccPcie0AuxRcg + 4);
  l.rchngRcg = rd(hw, kWinGcc, kGccPcie0RchngRcg);
  l.rchngCfg = rd(hw, kWinGcc, kGccPcie0RchngRcg + 4);
  l.pipeMux = rd(hw, kWinGcc, kGccPcie0PipeMux);
  l.branchOff = 0;
  for (unsigned i = 0; i < kBranchCount; ++i)
    if (branchOff(rd(hw, kWinGcc, kBranches[i].cbcr))) l.branchOff |= 1u << i;
  if ((l.vote8 & kVote8Bits) != kVote8Bits || (l.vote0 & kVote0Bits) != kVote0Bits) return failed(kStepClocks, kErrBranch);
  if (!(l.clkref & kTcsrClkRefEnable)) return failed(kStepClocks, kErrClkRef);
  if (!waitClear(hw, kWinGcc, kGccPcie0AuxRcg, kRcgUpdate, 100, 10, nullptr) ||
      !waitClear(hw, kWinGcc, kGccPcie0RchngRcg, kRcgUpdate, 100, 10, nullptr))
    return failed(kStepClocks, kErrRcg);
  // A hardware-gated branch may read CLK_OFF while idle; the pipe clock waits for the PHY. The rest must run.
  uint32_t mustRun = 0;
  for (unsigned i = 0; i < kBranchCount; ++i)
    if (!kBranches[i].hwcg && i != kPipe) mustRun |= 1u << i;
  if (l.branchOff & mustRun) return failed(kStepClocks, kErrBranch);
  if (stop(kStepClocks)) return kErrStopped;

  // The controller's and the PHY's block resets, pulsed.
  announce(hw, kStepResets, "the PCIe0 and PCIe0 PHY block resets, pulsed");
  const uint32_t bcrs[2] = {kResets[kResetPcie0].bcr, kResets[kResetPcie0Phy].bcr};
  for (unsigned i = 0; i < 2; ++i) {
    setBits(hw, kWinGcc, bcrs[i], kBcrAssert, "GCC block reset asserted");
    hw.udelay(hw.ctx, 1000);
    clearBits(hw, kWinGcc, bcrs[i], kBcrAssert, "GCC block reset released");
    hw.udelay(hw.ctx, 1000);
    l.bcr[i] = rd(hw, kWinGcc, bcrs[i]);
    if (resetAsserted(l.bcr[i])) return failed(kStepResets, kErrReset);
  }
  if (stop(kStepResets)) return kErrStopped;

  // The wrapper: a root complex, the PHY out of its test power-down, where the DBI and the iATU are, the slave address space, then
  // the power-management bits (PCI = mainline's cfg_1_9_0 sequence; PAGANI has the same offsets and order).
  announce(hw, kStepParf, "PARF: root complex, PHY test power-down off, DBI/iATU bases, slave space, PM bits");
  wr(hw, kWinParf, kParfDeviceType, kParfDeviceTypeRc, "PARF device type: root complex");
  clearBits(hw, kWinParf, kParfPhyCtrl, kParfPhyTestPwrDown, "PARF PHY control: test power-down off");
  wr(hw, kWinParf, kParfDbiBase, uint32_t(kDbi), "PARF DBI base");
  wr(hw, kWinParf, kParfDbiBaseHi, uint32_t(kDbi >> 32), "PARF DBI base high");
  wr(hw, kWinParf, kParfAtuBase, uint32_t(kAtu), "PARF iATU base");
  wr(hw, kWinParf, kParfAtuBaseHi, uint32_t(kAtu >> 32), "PARF iATU base high");
  wr(hw, kWinParf, kParfSlvSpace, 0, "PARF slave address space");
  wr(hw, kWinParf, kParfSlvSpaceHi, 0x80000000u, "PARF slave address space high");
  clearBits(hw, kWinParf, kParfSysCtrl, kParfSysCtrlMstrAxiClkEnable, "PARF system control");
  setBits(hw, kWinParf, kParfMhiClockResetCtrl, kParfMhiClockResetCtrlBypass, "PARF MHI clock reset control");
  clearBits(hw, kWinParf, kParfPmCtrl, kParfPmCtrlReqNotEntr, "PARF PM control");
  setBits(hw, kWinParf, kParfAxiMstrWrAddrHalt, kParfAxiMstrWrHaltEnable, "PARF AXI master write halt");
  l.parfDeviceType = rd(hw, kWinParf, kParfDeviceType);
  l.parfPhyCtrl = rd(hw, kWinParf, kParfPhyCtrl);
  l.parfDbiBase = rd(hw, kWinParf, kParfDbiBase);
  l.parfAtuBase = rd(hw, kWinParf, kParfAtuBase);
  l.parfSlvSpaceHi = rd(hw, kWinParf, kParfSlvSpaceHi);
  l.parfSysCtrl = rd(hw, kWinParf, kParfSysCtrl);
  if ((l.parfDeviceType & 0xf) != kParfDeviceTypeRc || (l.parfPhyCtrl & kParfPhyTestPwrDown) ||
      l.parfDbiBase != uint32_t(kDbi) || l.parfAtuBase != uint32_t(kAtu))
    return failed(kStepParf, kErrParf);
  if (stop(kStepParf)) return kErrStopped;

  // The PHY: the sequence up to the reset release, the pipe clock taken from the PHY, then the last two writes, then PHYSTATUS low.
  announce(hw, kStepPhy, "the QMP v7 PHY sequence, the pipe clock from the PHY, PHYSTATUS");
  if (opt.physeq) {
    for (unsigned i = 0; i + kPhyFrameTail < kPhySequenceCount; ++i) {
      wr(hw, kWinPhy, kPhySequence[i].offset, kPhySequence[i].value, "PHY sequence");
      ++l.phyWrites;
    }
  } else {
    wr(hw, kWinPhy, kPcs + kPcsPowerDown, 3, "PHY power-down control (frame only)");
    ++l.phyWrites;
  }
  wr(hw, kWinGcc, kGccPcie0PipeMux, (rd(hw, kWinGcc, kGccPcie0PipeMux) & ~3u) | kPipeMuxPhy, "GCC pcie0 pipe mux: the PHY");
  for (unsigned i = kPhySequenceCount - kPhyFrameTail; i < kPhySequenceCount; ++i) {
    wr(hw, kWinPhy, kPhySequence[i].offset, kPhySequence[i].value, "PHY sequence (reset release, start)");
    ++l.phyWrites;
  }
  hw.udelay(hw.ctx, 1000);
  const bool ready = waitClear(hw, kWinPhy, kPcs + kPcsStatus1, kPcsPhyStatus, 100, 100, &l.phyPolls);
  l.pcsStatus = rd(hw, kWinPhy, kPcs + kPcsStatus1);
  l.pipeMux = rd(hw, kWinGcc, kGccPcie0PipeMux);
  l.pipeCbcr = rd(hw, kWinGcc, kBranches[kPipe].cbcr);
  if (!ready) return failed(kStepPhy, kErrPhy);
  if (stop(kStepPhy)) return kErrStopped;

  // Which SMMU stream the endpoint's requests carry: the requester-ID hash table, used once the bypass bit is clear. The root port
  // (requester 0) hashes to entry 0 with offset 0, which the zeroed table already says.
  announce(hw, kStepSid, "PARF BDF-to-SID: requester 0x100 to stream 0x1401");
  for (unsigned i = 0; i < 256; ++i) wr(hw, kWinParf, kParfBdfToSidTable + 4 * i, 0, "PARF BDF-to-SID table cleared");
  clearBits(hw, kWinParf, kParfBdfToSidCfg, kParfBdfToSidBypass, "PARF BDF-to-SID: table, not bypass");
  l.sidHash = bdfHash(kEndpointRid);
  const uint32_t entry = uint32_t(kEndpointRid) << 16 | uint32_t(kStreamEndpoint - kStreamRoot) << 8;
  wr(hw, kWinParf, kParfBdfToSidTable + 4u * l.sidHash, entry, "PARF BDF-to-SID entry of the endpoint");
  l.sidEntry = rd(hw, kWinParf, kParfBdfToSidTable + 4u * l.sidHash);
  l.sidCfg = rd(hw, kWinParf, kParfBdfToSidCfg);
  if (l.sidEntry != entry || (l.sidCfg & kParfBdfToSidBypass)) return failed(kStepSid, kErrSid);
  if (stop(kStepSid)) return kErrStopped;

  // The root port: bus numbers 0/1/1, memory decoding and bus mastering, the window it forwards, and the target link speed.
  announce(hw, kStepRootPort, "the root port: bus numbers, memory window, target link speed");
  l.rootId = rd(hw, kWinDbi, kDbiId);
  wr(hw, kWinDbi, kDbiBusNumbers, 0x00010100u, "DBI bus numbers 0/1/1");
  l.memBaseLimit = memBaseLimit(kMem32, kMem32Bytes);
  wr(hw, kWinDbi, kDbiMemBaseLimit, l.memBaseLimit, "DBI memory base/limit of the 32-bit window");
  setBits(hw, kWinDbi, kDbiCommand, kCommandMemory | kCommandMaster, "DBI command: memory space, bus master");
  uint32_t cap = rd(hw, kWinDbi, kDbiCapPtr) & 0xfcu;
  for (unsigned guard = 0; cap >= 0x40 && cap < 0x100 && guard < 16; ++guard) {
    const uint32_t w = rd(hw, kWinDbi, cap);
    if ((w & 0xffu) == 0x10) { l.expCap = cap; break; }
    cap = (w >> 8) & 0xfcu;
  }
  const unsigned gen = opt.gen >= 1 && opt.gen <= kMaxGen ? opt.gen : kDefaultGen;
  if (l.expCap) {
    wr(hw, kWinDbi, l.expCap + kExpLinkCtl2, (rd(hw, kWinDbi, l.expCap + kExpLinkCtl2) & ~0xfu) | gen,
       "DBI link control 2: target link speed");
    l.linkCtl2 = rd(hw, kWinDbi, l.expCap + kExpLinkCtl2);
  }
  l.busNumbers = rd(hw, kWinDbi, kDbiBusNumbers);
  l.rootCommand = rd(hw, kWinDbi, kDbiCommand);
  if (l.busNumbers != 0x00010100u) return failed(kStepRootPort, kErrParf);
  if (stop(kStepRootPort)) return kErrStopped;

  // PERST# released at least 100 ms after it went low, then the LTSSM. Nothing sleeps between the release and the poll.
  announce(hw, kStepLtssm, "PERST# released (>= 100 ms low), then the LTSSM enabled");
  const uint64_t lowFor = hw.now_ms(hw.ctx) - p.perstAssertedMs;
  if (lowFor < 100) hw.msleep(hw.ctx, unsigned(100 - lowFor));
  l.perstLowMs = uint32_t(hw.now_ms(hw.ctx) - p.perstAssertedMs);
  setPin(hw, kPins[kPinPerst].gpio, pinConfig(-1, 0, 2, true), 1, "PERST# released");
  hw.udelay(hw.ctx, 1000);
  setBits(hw, kWinParf, kParfLtssm, kParfLtssmEnable, "PARF LTSSM enable");
  l.parfLtssm = rd(hw, kWinParf, kParfLtssm);
  if (!(l.parfLtssm & kParfLtssmEnable)) return failed(kStepLtssm, kErrParf);
  if (stop(kStepLtssm)) return kErrStopped;

  announce(hw, kStepLink, "link training to L0");
  const uint64_t start = hw.now_ms(hw.ctx);
  bool trained = false;
  for (unsigned ms = 0; ms < opt.linkMs; ++ms) {
    l.debug0 = rd(hw, kWinDbi, kPortDebug0);
    l.debug1 = rd(hw, kWinDbi, kPortDebug1);
    if ((l.debug1 & kPortDebug1LinkUp) && (l.debug0 & kLtssmMask) == kLtssmL0) { trained = true; break; }
    hw.msleep(hw.ctx, 1);
  }
  l.linkMs = uint32_t(hw.now_ms(hw.ctx) - start);
  if (l.expCap) l.linkStatus = rd(hw, kWinDbi, l.expCap + kExpLinkCtlSta);
  if (!trained) return failed(kStepLink, kErrLink);
  if (stop(kStepLink)) return kErrStopped;

  // The first request that leaves the SoC: outbound iATU region 0 turns the 1 MiB configuration window into type 0 accesses to
  // bus 1, device 0.
  announce(hw, kStepEndpoint, "iATU region 0 (configuration window), then the endpoint's identity");
  wr(hw, kWinAtu, kAtuBaseLo, uint32_t(kConfig), "iATU 0 base");
  wr(hw, kWinAtu, kAtuBaseHi, uint32_t(kConfig >> 32), "iATU 0 base high");
  wr(hw, kWinAtu, kAtuLimit, uint32_t(kConfig + 0xfffff), "iATU 0 limit");
  wr(hw, kWinAtu, kAtuUpperLimit, uint32_t((kConfig + 0xfffff) >> 32), "iATU 0 upper limit");
  wr(hw, kWinAtu, kAtuTargetLo, 1u << 24, "iATU 0 target: bus 1");
  wr(hw, kWinAtu, kAtuTargetHi, 0, "iATU 0 target high");
  wr(hw, kWinAtu, kAtuCtrl1, kAtuTypeCfg0, "iATU 0 type: configuration");
  wr(hw, kWinAtu, kAtuCtrl2, kAtuCtrl2Enable, "iATU 0 enable");
  unsigned atuPolls = 0;
  if (!waitSet(hw, kWinAtu, kAtuCtrl2, kAtuCtrl2Enable, 5, 10, &atuPolls)) return failed(kStepEndpoint, kErrAtu);
  l.atuCtrl2[0] = rd(hw, kWinAtu, kAtuCtrl2);
  for (unsigned i = 0; i < kEndpointAnswerMs / 10; ++i) {
    l.endpointId = rd(hw, kWinCfg, 0);
    ++l.endpointPolls;
    if (!idPending(l.endpointId)) break;
    hw.msleep(hw.ctx, 10);
  }
  if (!idIsEndpoint(l.endpointId)) return failed(kStepEndpoint, kErrEndpoint);
  l.endpointClass = rd(hw, kWinCfg, 0x08);
  l.endpointSubsys = rd(hw, kWinCfg, 0x2c);
  uint32_t ecap = rd(hw, kWinCfg, 0x34) & 0xfcu;
  for (unsigned guard = 0; ecap >= 0x40 && ecap < 0x100 && guard < 16; ++guard) {
    const uint32_t w = rd(hw, kWinCfg, ecap);
    if ((w & 0xffu) == 0x10) l.endpointCap = ecap;
    if ((w & 0xffu) == 0x05) l.endpointMsiCap = ecap;
    ecap = (w >> 8) & 0xfcu;
  }
  if (stop(kStepEndpoint)) return kErrStopped;

  // The memory window (iATU region 1, CPU = PCI addresses), then BAR0: sized by the all-ones probe, placed at the first address of
  // the window aligned to its own size, memory decoding and bus mastering on.
  announce(hw, kStepBar, "iATU region 1 (the memory window), BAR0's size and address, the endpoint's command");
  const uint32_t r1 = kAtuRegion;
  wr(hw, kWinAtu, r1 + kAtuBaseLo, uint32_t(kMem32), "iATU 1 base");
  wr(hw, kWinAtu, r1 + kAtuBaseHi, uint32_t(kMem32 >> 32), "iATU 1 base high");
  wr(hw, kWinAtu, r1 + kAtuLimit, uint32_t(kMem32 + kMem32Bytes - 1), "iATU 1 limit");
  wr(hw, kWinAtu, r1 + kAtuUpperLimit, uint32_t((kMem32 + kMem32Bytes - 1) >> 32), "iATU 1 upper limit");
  wr(hw, kWinAtu, r1 + kAtuTargetLo, uint32_t(kMem32), "iATU 1 target");
  wr(hw, kWinAtu, r1 + kAtuTargetHi, uint32_t(kMem32 >> 32), "iATU 1 target high");
  wr(hw, kWinAtu, r1 + kAtuCtrl1, kAtuTypeMem, "iATU 1 type: memory");
  wr(hw, kWinAtu, r1 + kAtuCtrl2, kAtuCtrl2Enable, "iATU 1 enable");
  if (!waitSet(hw, kWinAtu, r1 + kAtuCtrl2, kAtuCtrl2Enable, 5, 10, nullptr)) return failed(kStepBar, kErrAtu);
  l.atuCtrl2[1] = rd(hw, kWinAtu, r1 + kAtuCtrl2);
  const uint32_t saved = rd(hw, kWinCfg, 0x10);
  wr(hw, kWinCfg, 0x10, 0xffffffffu, "endpoint BAR0: size probe");
  l.barMask = rd(hw, kWinCfg, 0x10);
  wr(hw, kWinCfg, 0x10, saved, "endpoint BAR0: probe undone");
  const uint64_t size = barSize(l.barMask);
  const uint64_t at = barPlace(kMem32, kMem32Bytes, size);
  if (!size || !at) return failed(kStepBar, kErrBar);
  wr(hw, kWinCfg, 0x10, uint32_t(at) | (l.barMask & 0xfu), "endpoint BAR0: its address");   // the low nibble is read-only (the BAR's kind)
  if (barIs64(l.barMask)) wr(hw, kWinCfg, 0x14, 0, "endpoint BAR0 high");
  setBits(hw, kWinCfg, 0x04, kCommandMemory | kCommandMaster, "endpoint command: memory space, bus master");
  l.barValue = rd(hw, kWinCfg, 0x10);
  l.command = rd(hw, kWinCfg, 0x04);
  l.barAddress = at;
  l.barBytes = uint32_t(size);
  if ((l.barValue & ~0xfu) != uint32_t(at) || !(l.command & kCommandMemory) || !(l.command & kCommandMaster))
    return failed(kStepBar, kErrBar);
  l.step = kStepBar;
  return kErrNone;
}

}  // namespace pianowifi
