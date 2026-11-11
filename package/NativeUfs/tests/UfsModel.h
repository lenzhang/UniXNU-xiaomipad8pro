// SPDX-License-Identifier: BSD-2-Clause-Patent
// The controller and device model of the host tests: UFSHCI registers (IS write-one-to-clear, HCS with UPMCRS, the doorbells, AHIT,
// the UIC command registers with DME_GET of TX lane 0's FSM, DME_HIBERNATE_ENTER and EXIT), the request list in region A (even
// entries), command UPIUs (NOP OUT, query, SCSI: READ(10), WRITE(10), SYNCHRONIZE CACHE) and their faults.
// From ShengUfs b11de87 tests/RootCommands.cpp and UniXNU-Odin3 6d58c09 tests/RootCommands.cpp (the fault fixture, BSD-2-Clause-Patent,
// blu-sharky; ../UPSTREAM). UniXNU-piano: Hibern8 enter, the link's rule for a doorbell (a hibernated link leaves Hibern8 for a
// doorbell only with auto-hibernate on; with AHIT 0 the command never completes), the power commands' preconditions (no doorbell,
// AHIT 0), the order of events, the sleep hook.
#include "Bridge.h"
#include <array>
#include <algorithm>
#include <functional>

enum class Fault { None, Empty, Type, Tag, Lun, Query, Residual, Underflow, Oversize, SenseLength, CheckCondition, Timeout,
  StuckWithReply, Fatal, Stop, Start, HighBase, Busy, TaskSetFull, Aggregation,
  UicTimeout, UicResult, UicFatal, ExitTimeout, ExitDelayed, ExitStatus, StillHibern8, AutoHibernate,
  // UniXNU-piano: the power sequence
  EnterResult, EnterTimeout, EnterStatus, EnterNoChange, ExitFailOnce, ExitFailAlways, SyncCheck, SyncTimeout, NopTimeout };

struct Model {
  Fault fault = Fault::None;
  std::array<uint8_t, kDmaBytes> device {};
  std::array<uint32_t, 256> registers {};
  uint64_t now = 0;
  unsigned writes = 0, releases = 0, sleeps = 0;
  bool released = false, bringUp = false, exitBroken = false;   // exitBroken: every Hibern8 exit fails, whatever the fault
  uint32_t txState = 5;                        // TX lane 0's FSM: 1 HIBERN8, anything else an active lane
  unsigned getCommands = 0, enterCommands = 0, exitCommands = 0, exitFailures = 0;
  unsigned syncs = 0, nops = 0, doorbells = 0, doorbellInHibern8 = 0, autoExits = 0;
  uint64_t exitAt = 0;
  unsigned busyLeft = 0, lastSlot = 0;
  std::vector<uint8_t> written;
  std::vector<std::string> events;             // "get", "uic17", "uic18", "ahit=<hex>", "nop", "sync", "read", "write"
  std::function<void (uintptr_t)> onSleep;     // a thread would sleep on this event: the test plays the other thread
};
static Model m;

static std::string hex (uint32_t value) { char text[16]; snprintf (text, sizeof (text), "%x", value); return text; }
static uint32_t &reg (unsigned offset) { return m.registers.at (offset / 4); }

static void uic (uint32_t command)
{
  assert (!(reg (0x20) & 0x400));   // the stale completion was acknowledged
  if (m.bringUp) assert (reg (0x60) == 0 && reg (0x58) == 0 && m.releases == 0);
  if (command == 0x01) {
    ++m.getCommands;
    m.events.push_back ("get");
    assert (reg (0x94) == 0x00410000);
    if (m.fault == Fault::UicTimeout) return;
    reg (0x9c) = m.txState;
    reg (0x98) = m.fault == Fault::UicResult ? 1 : 0;
    reg (0x20) |= m.fault == Fault::UicFatal ? 0x20400 : 0x400;
    return;
  }
  // DME_HIBERNATE_ENTER / EXIT: nothing outstanding, and (the rule RootPower.h follows) auto-hibernate off.
  assert (reg (0x58) == 0 && reg (0x78) == 0);
  assert (reg (0x18) == 0);
  const auto upmcrs = [] (uint32_t value) { reg (0x30) = (reg (0x30) & ~0x700U) | (value << 8); };
  if (command == 0x17) {
    ++m.enterCommands;
    m.events.push_back ("uic17");
    assert (m.txState != 1 && !(reg (0x20) & 0x40));
    switch (m.fault) {
    case Fault::EnterResult: reg (0x98) = 1; reg (0x20) |= 0x400; return;                       // refused, the link unchanged
    case Fault::EnterTimeout: m.txState = 1; reg (0x98) = 0; reg (0x20) |= 0x400; return;       // in Hibern8, UHES never comes
    case Fault::EnterStatus: m.txState = 1; reg (0x98) = 0; reg (0x20) |= 0x440; upmcrs (5); return;   // PWR_FATAL_ERROR
    case Fault::EnterNoChange: reg (0x98) = 0; reg (0x20) |= 0x440; upmcrs (1); return;         // says done, lane still active
    default: m.txState = 1; reg (0x98) = 0; reg (0x20) |= 0x440; upmcrs (1); return;
    }
  }
  assert (command == 0x18);
  ++m.exitCommands;
  m.events.push_back ("uic18");
  assert (m.txState == 1 && !(reg (0x20) & 0x20));
  if (m.exitBroken || m.fault == Fault::ExitFailAlways || (m.fault == Fault::ExitFailOnce && !m.exitFailures)) {
    ++m.exitFailures;
    reg (0x98) = 0;
    reg (0x20) |= 0x400;   // UCCS, no UHXS: the wait for the link times out
    return;
  }
  reg (0x98) = 0;
  reg (0x20) |= (m.fault == Fault::ExitTimeout || m.fault == Fault::ExitDelayed) ? 0x400 : 0x420;
  if (m.fault == Fault::ExitDelayed) m.exitAt = m.now + 250000;
  upmcrs (m.fault == Fault::ExitStatus ? 2 : 1);
  if (m.fault != Fault::StillHibern8) m.txState = 5;
}

static void doorbell (uint32_t value)
{
  assert (m.released);
  m.released = false;
  // Exactly one doorbell bit, of an even request list entry (RootTransportLayout.h).
  assert (value && !(value & (value - 1)));
  const unsigned entry = unsigned (__builtin_ctz (value));
  assert (entry % 2 == 0 && entry / 2 < kRegionSlots);
  const unsigned slot = entry / 2;
  m.lastSlot = slot;
  ++m.doorbells;
  if (m.txState == 1) {
    // UniXNU-piano: the controller takes a hibernated link out for a doorbell only with auto-hibernate on.
    if (!reg (0x18)) { ++m.doorbellInHibern8; return; }
    m.txState = 5;
    ++m.autoExits;
  }
  // The entry points at this slot's command descriptor.
  const uint8_t *utrd = m.device.data () + ufsSlotUtrdOffset (slot);
  const uint64_t ucd = *reinterpret_cast<const uint32_t *> (utrd + 16) | uint64_t (*reinterpret_cast<const uint32_t *> (utrd + 20)) << 32;
  assert (ucd == gHost.phys + ufsSlotUcdOffset (slot));
  if (m.fault == Fault::Fatal) { reg (0x20) = 0x20000; return; }
  reg (0x58) = 0;
  if (m.fault == Fault::Timeout) return;
  uint8_t *cmd = m.device.data () + ufsSlotUcdOffset (slot), *rsp = cmd + kRspOffset;
  if (cmd[0] == 1) assert (cmd[3] == entry);   // SCSI task tag: the request list entry
  std::fill (rsp, rsp + 512, 0);
  const bool scsi = cmd[0] == 1, nop = cmd[0] == 0;
  const bool sync = scsi && cmd[16] == 0x35, write = scsi && cmd[16] == 0x2a, read = scsi && cmd[16] == 0x28;
  if (nop) {
    ++m.nops;
    m.events.push_back ("nop");
    if (m.fault == Fault::NopTimeout) { reg (0x58) = value; return; }
  }
  if (sync) {
    ++m.syncs;
    m.events.push_back ("sync");
    if (m.fault == Fault::SyncTimeout) { reg (0x58) = value; return; }
  }
  if (read) m.events.push_back ("read");
  if (write) {                                  // WRITE(10): keep what the data area held
    m.events.push_back ("write");
    const uint32_t n = uint32_t (cmd[23]) << 8 | cmd[24];
    m.written.assign (m.device.data () + ufsSlotDataOffset (slot), m.device.data () + ufsSlotDataOffset (slot) + n * 4096);
  } else {
    memset (m.device.data () + ufsSlotDataOffset (slot), 0x7b + slot, kDataMax);   // a write leaves its buffer
  }
  *reinterpret_cast<uint32_t *> (m.device.data () + ufsSlotUtrdOffset (slot) + 8) = 0;
  if (m.fault == Fault::Empty) return;
  memcpy (rsp, cmd, 32);
  rsp[0] = cmd[0] + 0x20;
  rsp[1] = 0;
  if (m.fault == Fault::StuckWithReply) reg (0x58) = value;
  if (scsi) putBe32 (rsp + 12, 0);
  if (cmd[0] == 0x16) putBe32 (rsp + 20, 0x11);
  if (m.fault == Fault::Type) rsp[0] = 0x3f;
  if (m.fault == Fault::Tag) ++rsp[3];
  if (m.fault == Fault::Lun) ++rsp[2];
  if (m.fault == Fault::Query) ++rsp[13];
  if (m.fault == Fault::Residual) putBe32 (rsp + 12, 4096);
  if (m.fault == Fault::Underflow) rsp[1] = 0x20;
  if (m.fault == Fault::Oversize) rsp[10] = 0xff;
  if (m.fault == Fault::SenseLength) { rsp[11] = 2; rsp[33] = 18; }
  if ((m.fault == Fault::Busy || m.fault == Fault::TaskSetFull) && m.busyLeft) { --m.busyLeft; rsp[7] = m.fault == Fault::Busy ? 0x08 : 0x28; }
  if (m.fault == Fault::CheckCondition || (m.fault == Fault::SyncCheck && sync)) {
    rsp[7] = 2; rsp[11] = 20; rsp[33] = 18; rsp[34] = 0x70;
    rsp[36] = 6; rsp[46] = 0x29; rsp[47] = 0;
  }
}

static uint64_t io (unsigned op, uint64_t address, uint64_t value, void *bytes)
{
  if (op == 0) {
    if (address == 0x20 && m.exitAt && m.now >= m.exitAt) { reg (0x20) |= 0x20; m.exitAt = 0; }
    return m.registers.at (address / 4);
  }
  if (op == 1) {
    ++m.writes;
    if (address == 0x20) { reg (0x20) &= ~uint32_t (value); return 0; }
    if (address == 0x18) {
      if (m.fault == Fault::AutoHibernate) return 0;   // the register ignores every write
      reg (0x18) = uint32_t (value);
      m.events.push_back ("ahit=" + hex (uint32_t (value)));
      return 0;
    }
    if ((address == 0x60 && ((m.fault == Fault::Stop && !value) || (m.fault == Fault::Start && value))) ||
        (address == 0x54 && m.fault == Fault::HighBase) || (address == 0x4c && m.fault == Fault::Aggregation)) return 0;
    m.registers.at (address / 4) = uint32_t (value);
    if (address == 0x90) uic (uint32_t (value));
    if (address == 0x58) doorbell (uint32_t (value));
    return 0;
  }
  if (op == 2 || op == 3) {
    assert (address >= gHost.phys && address - gHost.phys + value <= m.device.size ());
    uint8_t *at = m.device.data () + (address - gHost.phys);
    if (op == 2) { ++m.releases; memcpy (at, bytes, value); m.released = true; }
    else memcpy (bytes, at, value);
    return 0;
  }
  if (op == 4) return m.now++;
  if (op == 5) { ++m.sleeps; m.now += value; return 0; }
  if (op == 6) {
    assert (m.onSleep && "a thread would sleep with nobody to wake it");
    m.onSleep (uintptr_t (address));
    return 0;
  }
  return 0;
}

static void reset (Fault f = Fault::None)
{
  m = Model {};
  m.fault = f;
  reg (0x00) = (1U << 23) | 0x1f;   // CAP: auto-hibernate, 32 request slots
  reg (0x34) = 1;                    // HCE
  reg (0x30) = 15;                   // HCS: device present, both lists ready, UIC ready
  reg (0x60) = 1;                    // the request list runs
  ufs_test_reset (io, 0x8cb5e9000);
}

static void assertQuarantined ()
{
  assert (gHost.poisoned);
  std::array<uint8_t, kDmaBytes> old;
  memcpy (old.data (), memory, sizeof (memory));
  const unsigned oldWrites = m.writes, oldReleases = m.releases;
  uint8_t cdb[6] = {};
  uint32_t scsi = 0, value = 0xabcdef;
  assert (hostNop () != 0);
  assert (hostQueryRead (3, 2, &value) != 0 && value == 0xabcdef);
  assert (ufs_test_scsi (0, cdb, 6, false, 0, &scsi) != 0);
  assert (m.writes == oldWrites && m.releases == oldReleases);
  assert (!memcmp (old.data (), memory, sizeof (memory)));
}
