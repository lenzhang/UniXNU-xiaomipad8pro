// SPDX-License-Identifier: BSD-2-Clause-Patent
// The production command, slot and queue path against the fault model (UfsModel.h): ShengUfs b11de87's tests/RootCommands.cpp (the
// command path, the slots, quarantine) and UniXNU-Odin3 6d58c09's SM8750 link tests (MCQ guard, auto-hibernate off, Hibern8 exit,
// aggregation off), unchanged in what they assert. UniXNU-piano: the same tests now run with the queue hold compiled in and
// RootQueue.h's hostLinkCommand taking DME_HIBERNATE_ENTER too, which must change nothing here (native-ufs-suspend=0 is today's).
#include "UfsModel.h"

int main ()
{
  reset ();
  assert (hostNop () == 0);
  uint32_t value = 0;
  assert (hostQueryRead (3, 2, &value) == 0 && value == 0x11);
  std::array<uint8_t, 131072> out;
  assert (hostRead4k (0, 1, 32, out.data ()) == 0);
  assert (std::all_of (out.begin (), out.end (), [] (uint8_t x) { return x == 0x7b; }));
  assert (*reinterpret_cast<uint32_t *> (m.device.data () + 20) == 8);
  assert (*reinterpret_cast<uint32_t *> (m.device.data () + 4096 + 1024 + 4) == 8);
  assert (hostRead4k (0, 0xffffffffULL, 1, out.data ()) == 0);   // the last READ(10) LBA is valid
  auto oldWrites = m.writes;
  assert (hostRead4k (0, 0xffffffffULL, 2, out.data ()) == kIOReturnBadArgument && m.writes == oldWrites);
  for (Fault f : { Fault::Empty, Fault::Type, Fault::Tag, Fault::Lun, Fault::Residual, Fault::Underflow, Fault::Oversize,
                   Fault::SenseLength, Fault::Timeout, Fault::Fatal }) {
    reset (f);
    out.fill (0xa5);
    assert (hostRead4k (0, 1, 1, out.data ()) != 0);
    assert (std::all_of (out.begin (), out.end (), [] (uint8_t x) { return x == 0xa5; }));
    assertQuarantined ();
  }
  reset (Fault::Query);
  value = 0xabcdef;
  assert (hostQueryRead (3, 2, &value) != 0 && value == 0xabcdef);
  assertQuarantined ();
  reset (Fault::CheckCondition);
  out.fill (0xa5);
  assert (hostRead4k (0, 1, 1, out.data ()) == kIOReturnNotReady);
  assert (!gHost.poisoned && gRootDiagnostic.sense == 0x062900);
  assert (out[0] == 0xa5);
  m.fault = Fault::None;
  assert (hostRead4k (0, 1, 1, out.data ()) == 0);
  reset ();
  uint8_t cdb[17] = {};
  uint32_t scsi;
  assert (ufs_test_scsi (0, cdb, 17, false, 0, &scsi) == kIOReturnBadArgument);
  assert (ufs_test_scsi (0, cdb, 6, true, kDataMax + 4, &scsi) == kIOReturnBadArgument);
  assert (ufs_test_scsi (0, cdb, 6, false, 4096, &scsi) == kIOReturnBadArgument);   // no writes to media
  assert (m.writes == 0 && m.releases == 0);
  // Odin3: the MCQ guard (CAP bit 30 with MCQ enabled refuses the takeover; ESI alone does not).
  reset (); reg (0x00) = 0x40000000; reg (0x300) = 1; assert (!ufs_test_stop_queue () && m.writes == 0);
  reset (); reg (0x00) = 0x40000000; reg (0x300) = 2; assert (ufs_test_stop_queue ());
  reset (); reg (0x00) = 0x40000000; reg (0x300) = 0; assert (ufs_test_stop_queue ());
  for (unsigned offset : { 0x58U, 0x78U }) {
    reset ();
    reg (offset) = 1;
    assert (!ufs_test_stop_queue () && m.writes == 0);
  }
  reset (); reg (0x34) = 0; assert (!ufs_test_stop_queue () && m.writes == 0);
  reset (); reg (0x20) = 0x20000;
  assert (!ufs_test_stop_queue () && m.writes == 0);
  assert (hostNop () != 0 && m.writes == 0 && m.releases == 0);
  assertQuarantined ();
  reset (Fault::Stop); assert (!ufs_test_stop_queue () && m.now >= 100000);
  reset (Fault::Start); m.bringUp = true; assert (ufs_test_stop_queue ()); assert (!ufs_test_start_queue ());
  reset (Fault::HighBase); m.bringUp = true; assert (ufs_test_stop_queue ()); assert (!ufs_test_start_queue ());
  reset (); m.bringUp = true; assert (ufs_test_stop_queue () && ufs_test_start_queue ());
  assert (reg (0x50) == 0xcb5e9000 && reg (0x54) == 8);
  // Odin3: the SM8750 link handling before the first command.
  assert (m.getCommands == 1 && m.exitCommands == 0 && gRootDiagnostic.txBefore == 5);
  reset (); m.bringUp = true; m.txState = 1; reg (0x20) = 0x420; reg (0x18) = 0x1501;
  assert (ufs_test_stop_queue () && ufs_test_start_queue ());
  assert (m.getCommands == 2 && m.exitCommands == 1 && m.sleeps == 0);
  assert (gRootDiagnostic.txBefore == 1 && gRootDiagnostic.txAfter == 5);
  assert (gRootDiagnostic.linkAhit == 0x1501 && reg (0x18) == 0);
  m.bringUp = false;
  assert (hostNop () == 0);
  reset (Fault::ExitDelayed); m.bringUp = true; m.txState = 1;
  assert (ufs_test_stop_queue () && ufs_test_start_queue () && m.now >= 250000 && m.sleeps == 0);
  assert (m.exitCommands == 1 && gRootDiagnostic.txAfter == 5);
  for (Fault f : { Fault::UicTimeout, Fault::UicResult, Fault::UicFatal, Fault::ExitTimeout, Fault::ExitStatus, Fault::StillHibern8,
                   Fault::AutoHibernate }) {
    reset (f); m.bringUp = true; m.txState = 1; reg (0x20) = 0x420; reg (0x18) = 0x1501;
    assert (ufs_test_stop_queue () && !ufs_test_start_queue ());
    assert (reg (0x50) == 0 && reg (0x60) == 0 && m.releases == 0 && m.sleeps == 0);
    assert (gRootDiagnostic.result != 0);
    if (f == Fault::UicTimeout || f == Fault::ExitTimeout) assert (m.now >= 100000);
    if (f == Fault::ExitTimeout) assert (m.now >= 500000 && m.now < 500100);
    if (f == Fault::UicFatal) assert (reg (0x20) & 0x20000);
  }
  reset (); m.bringUp = true; assert (ufs_test_stop_queue ()); reg (0x30) &= ~8U;
  assert (!ufs_test_start_queue () && m.getCommands == 0 && m.exitCommands == 0 && m.releases == 0);
  reset (); m.bringUp = true; reg (0x4c) = 0x81001f20;
  assert (ufs_test_stop_queue () && ufs_test_start_queue ());
  assert (gRootDiagnostic.iacrBefore == 0x81001f20 && gRootDiagnostic.iacr == 0);
  m.bringUp = false;
  assert (reg (0x4c) == 0 && hostNop () == 0);
  reset (Fault::Aggregation); m.bringUp = true; reg (0x4c) = 0x81001f20;
  assert (ufs_test_stop_queue () && !ufs_test_start_queue ());
  assert (reg (0x50) == 0 && reg (0x60) == 0 && m.releases == 0);
  // Command slots: each slot's entry, descriptor, tag and data area; the pool; busy retries; the panic slot; one-slot fallback.
  for (unsigned slot = 0; slot < kRegionSlots; ++slot) {
    reset ();
    uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    uint32_t status = 0xffff;
    assert (ufs_test_scsi_slot (slot, 0, read10, 10, 4096, &status) == 0 && status == 0 && m.lastSlot == slot);
    assert (memory[ufsSlotDataOffset (slot)] == uint8_t (0x7b + slot) && memory[ufsSlotDataOffset (slot) + 4095] == uint8_t (0x7b + slot));
  }
  reset ();
  assert (gSlotCount == kRegionSlots && gSlotFree == (1U << (kRegionSlots - 1)) - 1);   // the last slot is the panic's
  assert (ufs_test_slot_take () == 0 && ufs_test_slot_take () == 1);
  assert (hostRead4k (0, 7, 1, out.data ()) == 0 && m.lastSlot == 2 && out[0] == uint8_t (0x7b + 2));   // the first free slot
  assert (gSlotFree == (1U << (kRegionSlots - 1)) - 1 - 3);                                    // given back after the command
  ufs_test_slot_give (0);
  ufs_test_slot_give (1);
  assert (hostStoreOpen (100, 64));
  { std::vector<uint8_t> data (8192, 0x55); assert (hostWrite4k (0, 110, 2, data.data ()) == 0 && m.written == data); }
  assert (hostSyncCache () == 0 && gSlotFree == (1U << (kRegionSlots - 1)) - 1 && m.syncs == 1);
  reset (Fault::Busy); m.busyLeft = 3; assert (hostRead4k (0, 1, 1, out.data ()) == 0 && !gHost.poisoned);
  reset (Fault::TaskSetFull); m.busyLeft = 5; assert (hostStoreOpen (100, 64));
  { std::vector<uint8_t> data (4096, 0x66); assert (hostWrite4k (0, 100, 1, data.data ()) == 0 && m.written == data); }
  reset (Fault::Busy); m.busyLeft = 1000; assert (hostRead4k (0, 1, 1, out.data ()) == kIOReturnNotReady && !gHost.poisoned);
  assert (gSlotFree == (1U << (kRegionSlots - 1)) - 1);                                         // a failed command gives its slot back
  reset (); gHostPanic = true; assert (hostRead4k (0, 1, 1, out.data ()) == 0 && m.lastSlot == kRegionSlots - 1);
  assert (gSlotFree == (1U << (kRegionSlots - 1)) - 1);
  reset (); ufs_test_slots (1); assert (gSlotCount == 1 && gSlotFree == 1);
  assert (hostRead4k (0, 1, 1, out.data ()) == 0 && m.lastSlot == 0 && gSlotFree == 1);
  gHostPanic = true; assert (hostRead4k (0, 1, 1, out.data ()) == 0 && m.lastSlot == 0);
  // native-ufs-suspend=0 and native-ufs-ahit absent (today's): nothing here ever held the queue, entered Hibern8 or set AHIT.
  assert (!gHostHeld && gHoldWaits == 0 && m.enterCommands == 0 && reg (0x18) == 0 && gPower.suspends == 0);
  puts ("PASS: command slots: per-slot entry, descriptor, tag and data; pool; busy and task-set-full retries; panic slot; one-slot fallback");
  puts ("PASS: command/queue path; malformed responses, bounds, short reads, sense, quarantine, queue refusal, MCQ guard, the SM8750 "
        "link (auto-hibernate off, Hibern8 exit and its faults, aggregation off): unchanged with the hold and Hibern8 enter compiled in");
}
